#include "moar.h"

/* Debug logging of EA. */
#define PEA_LOG 0
static void pea_log(char *fmt, ...) {
#if PEA_LOG
    va_list args;
    fprintf(stderr, "PEA: ");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
#endif
}

/* A materialization target register record (which register should we write a
 * materialized object into). */
struct MaterializationTarget {
    union {
        MVMSpeshOperand reg;
        MVMuint16 hyp_reg;
    };
    MVMuint8 is_hypothetical;
    struct MaterializationTarget *next;
};
typedef struct MaterializationTarget MaterializationTarget;

/* A transformation that we want to perform. */
#define TRANSFORM_DELETE_FASTCREATE     0
#define TRANSFORM_GETATTR_TO_SET        1
#define TRANSFORM_BINDATTR_TO_SET       2
#define TRANSFORM_DELETE_SET            3
#define TRANSFORM_GUARD_TO_SET          4
#define TRANSFORM_ADD_DEOPT_POINT       5
#define TRANSFORM_ADD_DEOPT_USAGE       6
#define TRANSFORM_PROF_ALLOCATED        7
#define TRANSFORM_DECOMPOSE_BIGINT_BI   8
#define TRANSFORM_UNBOX_BIGINT          9
#define TRANSFORM_MATERIALIZE           10
#define TRANSFORM_VIVIFY_TYPE           11
#define TRANSFORM_VIVIFY_CONCRETE       12
#define TRANSFORM_UNMATERIALIZE_BI      13
#define TRANSFORM_DECOMPOSE_BIGINT_REL  14
#define TRANSFORM_DECOMPOSE_BIGINT_UN   15
typedef struct {
    /* The allocation that this transform relates to eliminating. */
    MVMSpeshPEAAllocation *allocation;

    /* What kind of transform do we need to do? */
    MVMuint16 transform;

    /* Data per kind of transformation. */
    union {
        struct {
            /* The attribute instruction. */
            MVMSpeshIns *ins;
            /* If the referenced object didn't escape, and we replaced it,
             * we can just delete this operation. This is the allocation
             * to test if that's the case. */
            MVMSpeshPEAAllocation *target_allocation;
            /* The hypothetical register index to read or write. */
            MVMuint16 hypothetical_reg_idx;
        } attr;
        struct {
            /* The attribute instruction. */
            MVMSpeshIns *ins;
            /* The hypothetical register index to write. */
            MVMuint16 hypothetical_reg_idx;
            /* The sslot containing the type being vivified. */
            MVMuint16 type_sslot;
        } viv;
        struct {
            MVMSpeshIns *ins;
            MVMSTable *st;
        } fastcreate;
        struct {
            MVMSpeshIns *ins;
        } set;
        struct {
            /* The guard instruction. */
            MVMSpeshIns *ins;
            /* The the value guarded was a tracked allocation, then that
             * allocation. */
            MVMSpeshPEAAllocation *target_allocation;
        } guard;
        struct {
            MVMint32 deopt_point_idx;
            MVMuint16 target_reg;
        } dp;
        struct {
            MVMint32 deopt_point_idx;
            MVMuint16 hypothetical_reg_idx;
        } du;
        struct {
            MVMSpeshIns *ins;
        } prof;
        struct {
            MVMSpeshIns *ins;
            MVMuint16 hypothetical_reg_idx_a;
            MVMuint16 hypothetical_reg_idx_b;
            MVMuint16 obtain_offset_a;
            MVMuint16 obtain_offset_b;
            MVMuint16 replace_op;
        } decomp_op_bi;
        struct {
            MVMSpeshIns *ins;
            MVMuint16 hypothetical_reg_idx;
        } unbox_bi;
        struct {
            MVMSpeshIns *prior_to;
            MaterializationTarget *targets;
            MVMuint8 *used;
        } materialize;
        struct {
            MVMSpeshIns *ins;
            MVMSTable *st;
            MVMSpeshOperand unboxed;
        } unmat_bi;
        struct {
            MVMSpeshIns *ins;
            MVMSpeshPEAAllocation *dep_a;
            MVMSpeshPEAAllocation *dep_b;
            MVMuint16 hypothetical_reg_idx_a;
            MVMuint16 hypothetical_reg_idx_b;
            MVMuint16 obtain_offset_a;
            MVMuint16 obtain_offset_b;
            MVMuint16 replace_op;
        } decomp_rel_bi;
    };
} Transformation;

/* State held per basic block. */
typedef struct {
    /* The set of materialization transforms for this allocation. We keep
     * track of these so that if there is a usage of (typically an alias
     * of) the materialized value, we can add it to the set of registers
     * that we should materialize into. We use whether there is anything
     * in this vector as a way to know if we have allocated anything. The
     * reason there may be multiple is if we materialize on multiple sides
     * of a branch. */
    MVM_VECTOR_DECL(Transformation *, materializations);

    /* Which of the object's attributes have been used? Used for tracing
     * auto-viv. */
    MVMuint8 *used;

    /* Was the object seen by the time this basic block was reached?
     * Used to disregard basic blocks in a merge where the object
     * could not possibly have been visible, so we don't get spurious
     * materializations or irreplaceable status. */
    MVMuint8 seen;
} BBAllocationState;
typedef struct {
    /* The allocation state of tracked objects; during analysis of the
     * basic block, this is "as it stands", after processing it's the state
     * things were in by the end of that basic block's processing. */
    MVM_VECTOR_DECL(BBAllocationState, alloc_state);

    /* Transformations to apply. */
    MVM_VECTOR_DECL(Transformation *, transformations);
} BBState;

/* Shadow facts are used to track hypothetical extra information about an SSA
 * value. We hold them separately from the real facts, since they may not end
 * up applying (e.g. in the case of a loop where we have to iterate to a fixed
 * point). They can be indexed in two ways: by a hypothetical register ID or
 * by a concrete register ID (the former used for registers that we will only
 * create if we really do scalar replacement). */
typedef struct {
    MVMuint16 is_hypothetical;
    MVMuint16 hypothetical_reg_idx;
    MVMuint16 concrete_orig;
    MVMuint16 concrete_i;
    MVMSpeshFacts facts;
} ShadowFact;

/* A tracked register is one that is either the target of an allocation or
 * aliasing an allocation. We map it to the allocation tracking info. */
typedef struct {
    /* The register that is tracked. */
    MVMSpeshOperand reg;

    /* The allocation that is tracked there. */
    MVMSpeshPEAAllocation *allocation;
} TrackedRegister;

/* State we hold during the entire partial escape analysis process. */
typedef struct {
    /* The allocations that we are tracking. The indices in this match up
     * with the index field in a MVMSpeshPEAAllocation, and thus those used
     * in the per-basic-block materialization state too. */
    MVM_VECTOR_DECL(MVMSpeshPEAAllocation *, tracked_allocations);

    /* The latest temporary register index. We use these indices before we
     * really allocate temporary registers. */
    MVMuint16 latest_hypothetical_reg_idx;

    /* The actual temporary registers allocated, matching the hypotheticals
     * above. */
    MVMuint16 *attr_regs;

    /* State held per basic block. */
    BBState *bb_states;

    /* Shadow facts. */
    MVM_VECTOR_DECL(ShadowFact, shadow_facts);

    /* Tracked registers. */
    MVM_VECTOR_DECL(TrackedRegister, tracked_registers);

    /* The reverse postorder sort of the graph. */
    MVMSpeshBB **rpo;
} GraphState;

/* Turns a flattened-in STable into a register type to allocate, if possible.
 * Should it not be possible, returns a negative value. If passed NULL (which
 * indicates a reference type), then returns MVM_reg_obj. */
MVMint32 flattened_type_to_register_kind(MVMThreadContext *tc, MVMSTable *st) {
    if (st) {
        if (st->REPR->ID == MVM_REPR_ID_P6bigint) {
            return MVM_reg_obi;
        }
        else {
            const MVMStorageSpec *ss = st->REPR->get_storage_spec(tc, st);
            switch (ss->boxed_primitive) {
                case MVM_STORAGE_SPEC_BP_INT:
                    if (ss->bits == 64 && !ss->is_unsigned)
                        return MVM_reg_int64;
                    break;
                case MVM_STORAGE_SPEC_BP_NUM:
                    if (ss->bits == 64)
                        return MVM_reg_num64;
                    break;
                case MVM_STORAGE_SPEC_BP_STR:
                    return MVM_reg_str;
            }
            return -1;
        }
    }
    else {
        return MVM_reg_obj;
    }
}

/* Finds the hypothetical register holding a boxed big integer. */
static MVMuint16 find_bigint_register(MVMThreadContext *tc, MVMSpeshPEAAllocation *alloc) {
    MVMSTable *st = alloc->type->st;
    if (st->REPR->ID == MVM_REPR_ID_P6opaque) {
        MVMP6opaqueREPRData *repr_data = (MVMP6opaqueREPRData *)st->REPR_data;
        MVMuint32 i;
        for (i = 0; i < repr_data->num_attributes; i++) {
            MVMuint16 kind = flattened_type_to_register_kind(tc, repr_data->flattened_stables[i]);
            if (kind == MVM_reg_obi)
                return alloc->hypothetical_attr_reg_idxs[i];
        }
        MVM_panic(1, "PEA: no big integer attribute found in find_bigint_register");
    }
    else {
        MVM_panic(1, "PEA: non-P6opaque type in find_bigint_register");
    }
}

/* Gets, allocating if needed, the deopt materialization info index of a
 * particular tracked object. */
static MVMuint16 get_deopt_materialization_info(MVMThreadContext *tc, MVMSpeshGraph *g,
                                                GraphState *gs, MVMSpeshPEAAllocation *alloc) {
    if (alloc->has_deopt_materialization_idx) {
        return alloc->deopt_materialization_idx;
    }
    else {
        MVMSpeshPEAMaterializeInfo mi;

        /* Build up information about registers containing attribute data. */
        MVMP6opaqueREPRData *repr_data = (MVMP6opaqueREPRData *)alloc->type->st->REPR_data;
        MVMuint32 num_attrs = repr_data->num_attributes;
        MVMuint16 *attr_regs;
        if (num_attrs > 0) {
            MVMuint32 i;
            attr_regs = MVM_malloc(num_attrs * sizeof(MVMuint16));
            for (i = 0; i < num_attrs; i++)
                attr_regs[i] = gs->attr_regs[alloc->hypothetical_attr_reg_idxs[i]];
        }
        else {
            attr_regs = NULL;
        }

        /* Set up and add materialization info. */
        mi.stable_sslot = MVM_spesh_add_spesh_slot_try_reuse(tc, g, (MVMCollectable *)alloc->type->st);
        mi.num_attr_regs = num_attrs;
        mi.attr_regs = attr_regs;
        alloc->deopt_materialization_idx = MVM_VECTOR_ELEMS(g->deopt_pea.materialize_info);
        alloc->has_deopt_materialization_idx = 1;
        MVM_VECTOR_PUSH(g->deopt_pea.materialize_info, mi);

        return alloc->deopt_materialization_idx;
    }
}

/* Resolves a register in a materialization target into a concrete register
 * (it may need no resolution). */
static MVMSpeshOperand resolve_materialization_target(MVMThreadContext *tc, MVMSpeshGraph *g,
        GraphState *gs, MaterializationTarget *target) {
    if (target->is_hypothetical) {
        MVMSpeshOperand result;
        result.reg.orig = gs->attr_regs[target->hyp_reg];
        result.reg.i = MVM_spesh_manipulate_get_current_version(tc, g, result.reg.orig);
        return result;
    }
    else {
        return target->reg;
    }
}

/* We should not stick a materialization in an args sequence; insert it
 * prior to that. */
MVMSpeshIns * find_materialization_insertion_point(MVMThreadContext *tc, MVMSpeshIns *ins) {
    while (ins) {
        switch (ins->info->opcode) {
            case MVM_OP_arg_i:
            case MVM_OP_arg_n:
            case MVM_OP_arg_s:
            case MVM_OP_arg_o:
            case MVM_OP_argconst_i:
            case MVM_OP_argconst_n:
            case MVM_OP_argconst_s:
                ins = ins->prev;
                break;
            default:
                return ins;
        }
    }
    MVM_oops(tc, "Spesh PEA: failed to find materialization insertion point");
}

/* Emit the materialization of an object into the specified register. */
static void emit_materialization(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb,
                                 MVMSpeshIns *prior_to, MVMSpeshOperand target,
                                 GraphState *gs, MVMSpeshPEAAllocation *alloc,
                                 MVMuint8 *used) {
    /* Lookup type information. */
    MVMSTable *st = STABLE(alloc->type);
    MVMP6opaqueREPRData *repr_data = (MVMP6opaqueREPRData *)st->REPR_data;
    MVMuint32 num_attrs = repr_data->num_attributes;
    MVMuint32 i, int_cache_type_idx;

    /* If it's a big integer boxing with a single attribute, then we can use
     * the materialize op that goes via the integer cache, to avoid doing the
     * allocation in some cases. */
    if (alloc->bigint && num_attrs == 1 &&
            (int_cache_type_idx = MVM_intcache_type_index(tc, st->WHAT)) >= 0) {
        MVMSpeshIns *materialize = MVM_spesh_alloc(tc, g, sizeof(MVMSpeshIns));
        materialize->info = MVM_op_get_op(MVM_OP_sp_materialize_bi);
        materialize->operands = MVM_spesh_alloc(tc, g, 6 * sizeof(MVMSpeshOperand));
        materialize->operands[0] = target;
        materialize->operands[1].lit_i16 = st->size;
        materialize->operands[2].lit_i16 = MVM_spesh_add_spesh_slot(tc, g, (MVMCollectable *)st);
        materialize->operands[3].lit_i16 = sizeof(MVMObject) + repr_data->attribute_offsets[0];
        materialize->operands[4].reg.orig = gs->attr_regs[alloc->hypothetical_attr_reg_idxs[0]];
        materialize->operands[4].reg.i = MVM_spesh_manipulate_get_current_version(tc, g,
                materialize->operands[4].reg.orig);
        materialize->operands[5].lit_i16 = int_cache_type_idx;
        MVM_spesh_get_facts(tc, g, materialize->operands[0])->writer = materialize;
        MVM_spesh_usages_add_by_reg(tc, g, materialize->operands[4], materialize);
        MVM_spesh_manipulate_insert_ins(tc, bb, prior_to->prev, materialize);
        MVM_spesh_graph_add_comment(tc, g, materialize, "Materialization of scalar-replaced attribute");
    }
    else {
        /* Emit a fastcreate instruction to allocate the object. */
        MVMSpeshIns *fastcreate = MVM_spesh_alloc(tc, g, sizeof(MVMSpeshIns));
        fastcreate->info = MVM_op_get_op(MVM_OP_sp_fastcreate);
        fastcreate->operands = MVM_spesh_alloc(tc, g, 3 * sizeof(MVMSpeshOperand));
        fastcreate->operands[0] = target;
        fastcreate->operands[1].lit_i16 = st->size;
        fastcreate->operands[2].lit_i16 = MVM_spesh_add_spesh_slot(tc, g, (MVMCollectable *)st);
        MVM_spesh_get_facts(tc, g, fastcreate->operands[0])->writer = fastcreate;
        MVM_spesh_manipulate_insert_ins(tc, bb, prior_to->prev, fastcreate);
        MVM_spesh_graph_add_comment(tc, g, fastcreate, "Materialization of scalar-replaced attribute");

        /* Bind each of the attributes into place, provided it was written already. */
        for (i = 0; i < num_attrs; i++) {
            if (used[i]) {
                /* Allocate instruction and determine type of bind instruction we will
                 * need. */
                MVMSpeshIns *bind = MVM_spesh_alloc(tc, g, sizeof(MVMSpeshIns));
                bind->operands = MVM_spesh_alloc(tc, g, 3 * sizeof(MVMSpeshOperand));
                switch (flattened_type_to_register_kind(tc, repr_data->flattened_stables[i])) {
                    case MVM_reg_obj:
                        bind->info = MVM_op_get_op(MVM_OP_sp_bind_o);
                        break;
                    case MVM_reg_str:
                        bind->info = MVM_op_get_op(MVM_OP_sp_bind_s_nowb);
                        break;
                    case MVM_reg_int64:
                        bind->info = MVM_op_get_op(MVM_OP_sp_bind_i64);
                        break;
                    case MVM_reg_num64:
                        bind->info = MVM_op_get_op(MVM_OP_sp_bind_n);
                        break;
                    case MVM_reg_obi:
                        bind->info = MVM_op_get_op(MVM_OP_sp_takewrite_bi);
                        break;
                    default:
                        MVM_oops(tc, "Unimplemented attribute kind in materialization");
                }

                /* Set offset, target, and source registers. */
                bind->operands[0] = target;
                bind->operands[1].lit_i16 = sizeof(MVMObject) + repr_data->attribute_offsets[i];
                bind->operands[2].reg.orig = gs->attr_regs[alloc->hypothetical_attr_reg_idxs[i]];
                bind->operands[2].reg.i = MVM_spesh_manipulate_get_current_version(tc, g,
                        bind->operands[2].reg.orig);
                MVM_spesh_usages_add_by_reg(tc, g, bind->operands[0], bind);
                MVM_spesh_usages_add_by_reg(tc, g, bind->operands[2], bind);

                /* Insert the bind instruction. */
                MVM_spesh_manipulate_insert_ins(tc, bb, prior_to->prev, bind);
            }
        }
    }
}

/* Allocates concrete registers for a scalar replacemnet. */
static void allocate_concrete_registers(MVMThreadContext *tc, MVMSpeshGraph *g,
        GraphState *gs, MVMSpeshPEAAllocation *alloc) {
    MVMP6opaqueREPRData *repr_data = (MVMP6opaqueREPRData *)alloc->type->st->REPR_data;
    MVMuint32 i;
    for (i = 0; i < repr_data->num_attributes; i++) {
        MVMuint32 idx = alloc->hypothetical_attr_reg_idxs[i];
        gs->attr_regs[idx] = MVM_spesh_manipulate_get_unique_reg(tc, g,
            flattened_type_to_register_kind(tc, repr_data->flattened_stables[i]));
    }
}

/* Apply a transformation to the graph. */
static void apply_transform(MVMThreadContext *tc, MVMSpeshGraph *g, GraphState *gs,
        MVMSpeshBB *bb, Transformation *t) {
    /* Don't apply if we discovered this allocation wasn't possible to scalar
     * replace. */
    if (t->allocation && t->allocation->irreplaceable)
        return;

    /* Otherwise, go by the type of transform. */
    switch (t->transform) {
        case TRANSFORM_DELETE_FASTCREATE: {
            MVMSTable *st = t->fastcreate.st;
            MVMSpeshPEAAllocation *alloc = t->allocation;
            MVMSpeshIns *ins = t->fastcreate.ins;
            allocate_concrete_registers(tc, g, gs, alloc);
            pea_log("OPT: eliminated an allocation of %s into r%d(%d)",
                    st->debug_name, ins->operands[0].reg.orig,
                    ins->operands[0].reg.i);
            MVM_spesh_manipulate_delete_ins(tc, g, bb, ins);
            break;
        }
        case TRANSFORM_GETATTR_TO_SET: {
            MVMSpeshIns *ins = t->attr.ins;
            if (t->attr.target_allocation && !t->attr.target_allocation->irreplaceable) {
                /* Read of replaced object from replaced object; nothing to
                 * do at runtime. */
                MVM_spesh_manipulate_delete_ins(tc, g, bb, t->set.ins);
            }
            else {
                MVM_spesh_usages_delete_by_reg(tc, g, ins->operands[1], ins);
                ins->info = MVM_op_get_op(MVM_OP_set);
                ins->operands[1].reg.orig = gs->attr_regs[t->attr.hypothetical_reg_idx];
                ins->operands[1].reg.i = MVM_spesh_manipulate_get_current_version(tc, g,
                    ins->operands[1].reg.orig);
                MVM_spesh_usages_add_by_reg(tc, g, ins->operands[1], ins);
                MVM_spesh_graph_add_comment(tc, g, ins, "read of scalar-replaced attribute");
            }
            break;
        }
        case TRANSFORM_BINDATTR_TO_SET: {
            MVMSpeshIns *ins = t->attr.ins;
            if (t->attr.target_allocation && !t->attr.target_allocation->irreplaceable) {
                /* Write of replaced object into replaced object; nothing to
                 * do at runtime. */
                MVM_spesh_manipulate_delete_ins(tc, g, bb, t->set.ins);
            }
            else {
                MVM_spesh_usages_delete_by_reg(tc, g, ins->operands[0], ins);
                ins->info = MVM_op_get_op(MVM_OP_set);
                ins->operands[0].reg.orig = gs->attr_regs[t->attr.hypothetical_reg_idx];
                /* This new_version handling assumes linear code with no flow
                 * control. We need to revisit it later, probably by not caring
                 * about versions here and then placing versions and PHIs as
                 * needed after this operation. However, when we'll also have
                 * to update usages at that point too. */
                ins->operands[0] = MVM_spesh_manipulate_new_version(tc, g,
                    ins->operands[0].reg.orig);
                ins->operands[1] = ins->operands[2];
                MVM_spesh_get_facts(tc, g, ins->operands[0])->writer = ins;
                MVM_spesh_graph_add_comment(tc, g, ins, "write of scalar-replaced attribute");
            }
            break;
        }
        case TRANSFORM_DELETE_SET:
            MVM_spesh_manipulate_delete_ins(tc, g, bb, t->set.ins);
            break;
        case TRANSFORM_GUARD_TO_SET: {
            if (t->guard.target_allocation && !t->guard.target_allocation->irreplaceable) {
                /* If we guard an object whose allocation was elimianted, then we can
                 * drop the instruction entirely. */
                MVM_spesh_manipulate_delete_ins(tc, g, bb, t->guard.ins);
                pea_log("OPT: eliminated a guard instruction");
            }
            else {
                MVMSpeshIns *ins = t->guard.ins;
                ins->info = MVM_op_get_op(MVM_OP_set);
                MVM_spesh_graph_add_comment(tc, g, ins, "guard eliminated by scalar replacement");
                pea_log("OPT: rewrote a guard instruction into a set");
            }
            break;
        }
        case TRANSFORM_ADD_DEOPT_POINT: {
            MVMSpeshPEADeoptPoint dp;
            dp.deopt_point_idx = t->dp.deopt_point_idx;
            dp.materialize_info_idx = get_deopt_materialization_info(tc, g, gs, t->allocation);
            dp.target_reg = t->dp.target_reg;
            MVM_VECTOR_PUSH(g->deopt_pea.deopt_point, dp);
            break;
        }
        case TRANSFORM_ADD_DEOPT_USAGE: {
            MVMSpeshOperand used;
            used.reg.orig = gs->attr_regs[t->du.hypothetical_reg_idx];
            used.reg.i = MVM_spesh_manipulate_get_current_version(tc, g, used.reg.orig);
            MVM_spesh_usages_add_deopt_usage_by_reg(tc, g, used, t->du.deopt_point_idx);
            break;
        }
        case TRANSFORM_PROF_ALLOCATED: {
            MVMSpeshIns *ins = t->prof.ins;
            MVM_spesh_usages_delete_by_reg(tc, g, ins->operands[0], ins);
            ins->info = MVM_op_get_op(MVM_OP_prof_replaced);
            ins->operands[0].lit_i16 = MVM_spesh_add_spesh_slot_try_reuse(tc, g,
                    (MVMCollectable *)STABLE(t->allocation->type));
            break;
        }
        case TRANSFORM_DECOMPOSE_BIGINT_BI:
        case TRANSFORM_DECOMPOSE_BIGINT_UN: {
            /* Prepend instructions to read big integer out of box if needed. */
            MVMSpeshOperand a, b;
            MVMSpeshIns *ins = t->decomp_op_bi.ins;
            if (t->decomp_op_bi.obtain_offset_a) {
                MVMuint32 hyp_reg_idx = t->decomp_op_bi.hypothetical_reg_idx_a;
                MVMSpeshIns *get_ins = MVM_spesh_alloc(tc, g, sizeof(MVMSpeshIns));
                get_ins->info = MVM_op_get_op(MVM_OP_sp_get_bi);
                get_ins->operands = MVM_spesh_alloc(tc, g, 3 * sizeof(MVMSpeshOperand));
                gs->attr_regs[hyp_reg_idx] = MVM_spesh_manipulate_get_unique_reg(tc, g, MVM_reg_rbi);
                a = get_ins->operands[0] = MVM_spesh_manipulate_new_version(tc, g,
                        gs->attr_regs[hyp_reg_idx]);
                get_ins->operands[1] = ins->operands[1];
                get_ins->operands[2].lit_ui16 = t->decomp_op_bi.obtain_offset_a;
                MVM_spesh_get_facts(tc, g, get_ins->operands[0])->writer = get_ins;
                MVM_spesh_usages_add_by_reg(tc, g, get_ins->operands[1], get_ins);
                MVM_spesh_manipulate_insert_ins(tc, bb, ins->prev, get_ins);
            }
            else {
                a.reg.orig = gs->attr_regs[t->decomp_op_bi.hypothetical_reg_idx_a];
                a.reg.i = MVM_spesh_manipulate_get_current_version(tc, g, a.reg.orig);
            }
            if (t->transform == TRANSFORM_DECOMPOSE_BIGINT_BI) {
                if (t->decomp_op_bi.obtain_offset_b) {
                    MVMuint32 hyp_reg_idx = t->decomp_op_bi.hypothetical_reg_idx_b;
                    MVMSpeshIns *get_ins = MVM_spesh_alloc(tc, g, sizeof(MVMSpeshIns));
                    get_ins->info = MVM_op_get_op(MVM_OP_sp_get_bi);
                    get_ins->operands = MVM_spesh_alloc(tc, g, 3 * sizeof(MVMSpeshOperand));
                    gs->attr_regs[hyp_reg_idx] = MVM_spesh_manipulate_get_unique_reg(tc, g, MVM_reg_rbi);
                    b = get_ins->operands[0] = MVM_spesh_manipulate_new_version(tc, g,
                            gs->attr_regs[hyp_reg_idx]);
                    get_ins->operands[1] = ins->operands[2];
                    get_ins->operands[2].lit_ui16 = t->decomp_op_bi.obtain_offset_b;
                    MVM_spesh_get_facts(tc, g, get_ins->operands[0])->writer = get_ins;
                    MVM_spesh_usages_add_by_reg(tc, g, get_ins->operands[1], get_ins);
                    MVM_spesh_manipulate_insert_ins(tc, bb, ins->prev, get_ins);
                }
                else {
                    b.reg.orig = gs->attr_regs[t->decomp_op_bi.hypothetical_reg_idx_b];
                    b.reg.i = MVM_spesh_manipulate_get_current_version(tc, g, b.reg.orig);
                }
            }

            /* Allocate concrete registers for the target bigint. */
            allocate_concrete_registers(tc, g, gs, t->allocation);

            /* Now, transform the instruction itself. */
            pea_log("OPT: big integer result of %s unboxed", ins->info->name);
            MVM_spesh_usages_delete_by_reg(tc, g, ins->operands[1], ins);
            MVM_spesh_usages_delete_by_reg(tc, g, ins->operands[2], ins);
            if (t->transform == TRANSFORM_DECOMPOSE_BIGINT_BI)
                MVM_spesh_usages_delete_by_reg(tc, g, ins->operands[3], ins);
            ins->info = MVM_op_get_op(t->decomp_op_bi.replace_op);
            ins->operands[0] = MVM_spesh_manipulate_new_version(tc, g,
                    gs->attr_regs[find_bigint_register(tc, t->allocation)]);
            ins->operands[1] = a;
            MVM_spesh_usages_add_by_reg(tc, g, ins->operands[1], ins);
            if (t->transform == TRANSFORM_DECOMPOSE_BIGINT_BI) {
                ins->operands[2] = b;
                MVM_spesh_usages_add_by_reg(tc, g, ins->operands[2], ins);
            }
            MVM_spesh_get_facts(tc, g, ins->operands[0])->writer = ins;
            MVM_spesh_graph_add_comment(tc, g, ins, "big integer op unboxed by scalar replacement");
            break;
        }
        case TRANSFORM_UNBOX_BIGINT: {
            MVMSpeshIns *ins = t->unbox_bi.ins;
            MVM_spesh_usages_delete_by_reg(tc, g, ins->operands[1], ins);
            ins->info = MVM_op_get_op(MVM_OP_sp_unbox_bi);
            ins->operands[1].reg.orig = gs->attr_regs[t->unbox_bi.hypothetical_reg_idx];
            ins->operands[1].reg.i = MVM_spesh_manipulate_get_current_version(tc, g,
                ins->operands[1].reg.orig);
            MVM_spesh_usages_add_by_reg(tc, g, ins->operands[1], ins);
            MVM_spesh_graph_add_comment(tc, g, ins, "unbox of scalar-replaced boxed bigint");
            pea_log("OPT: rewrote an integer unbox to use unboxed big integer");
            break;
        }
        case TRANSFORM_MATERIALIZE: {
            /* Check that we actually need to materialize (have a target). */
            MaterializationTarget *initial_target = t->materialize.targets;
            if (initial_target) {
                MaterializationTarget *alias_target = initial_target->next;
                MVMSpeshIns *prior_to = t->materialize.prior_to;
                MVMuint8 *used = t->materialize.used;
                MVMSpeshOperand target_reg = resolve_materialization_target(tc, g,
                        gs, initial_target);
                emit_materialization(tc, g, bb,
                        find_materialization_insertion_point(tc, prior_to),
                        target_reg, gs, t->allocation, used);
                while (alias_target) {
                    MVMSpeshIns *set = MVM_spesh_alloc(tc, g, sizeof(MVMSpeshIns));
                    set->info = MVM_op_get_op(MVM_OP_set);
                    set->operands = MVM_spesh_alloc(tc, g, 2 * sizeof(MVMSpeshOperand));
                    set->operands[0] = resolve_materialization_target(tc, g, gs, alias_target);
                    set->operands[1] = target_reg;
                    MVM_spesh_get_facts(tc, g, set->operands[0])->writer = set;
                    MVM_spesh_usages_add_by_reg(tc, g, set->operands[1], set);
                    MVM_spesh_manipulate_insert_ins(tc, bb, prior_to->prev, set);
                    alias_target = alias_target->next;
                }
            }
            else {
                pea_log("OPT: prevented pointless materialization of %s",
                        t->allocation->type->st->debug_name);
            }
            break;
        }
        case TRANSFORM_VIVIFY_TYPE:
        case TRANSFORM_VIVIFY_CONCRETE: {
            /* Prepend a lookup of the type object. */
            MVMuint16 attr_reg = gs->attr_regs[t->viv.hypothetical_reg_idx];
            MVMSpeshIns *type_ins = MVM_spesh_alloc(tc, g, sizeof(MVMSpeshIns));
            type_ins->info = MVM_op_get_op(MVM_OP_sp_getspeshslot);
            type_ins->operands = MVM_spesh_alloc(tc, g, 2 * sizeof(MVMSpeshOperand));
            type_ins->operands[0] = MVM_spesh_manipulate_new_version(tc, g, attr_reg);
            type_ins->operands[1].lit_i16 = t->viv.type_sslot;
            MVM_spesh_get_facts(tc, g, type_ins->operands[0])->writer = type_ins;
            MVM_spesh_manipulate_insert_ins(tc, bb, t->viv.ins->prev, type_ins);

            /* If it's a concrete vivification, insert the clone. */
            if (t->transform == TRANSFORM_VIVIFY_CONCRETE) {
                MVMSpeshIns *clone_ins = MVM_spesh_alloc(tc, g, sizeof(MVMSpeshIns));
                clone_ins->info = MVM_op_get_op(MVM_OP_clone);
                clone_ins->operands = MVM_spesh_alloc(tc, g, 2 * sizeof(MVMSpeshOperand));
                clone_ins->operands[0] = MVM_spesh_manipulate_new_version(tc, g, attr_reg);
                clone_ins->operands[1] = type_ins->operands[0];
                MVM_spesh_get_facts(tc, g, clone_ins->operands[0])->writer = clone_ins;
                MVM_spesh_usages_add_by_reg(tc, g, clone_ins->operands[1], clone_ins);
                MVM_spesh_manipulate_insert_ins(tc, bb, t->viv.ins->prev, clone_ins);
            }

            /* Transform the read into a set. */
            MVM_spesh_usages_delete_by_reg(tc, g, t->viv.ins->operands[1], t->viv.ins);
            t->viv.ins->info = MVM_op_get_op(MVM_OP_set);
            t->viv.ins->operands[1].reg.orig = attr_reg;
            t->viv.ins->operands[1].reg.i = MVM_spesh_manipulate_get_current_version(tc, g, attr_reg);
            MVM_spesh_usages_add_by_reg(tc, g, t->viv.ins->operands[1], t->viv.ins);
            MVM_spesh_graph_add_comment(tc, g, t->viv.ins, "auto-viv of scalar-replaced attribute");
            break;
        }
        case TRANSFORM_UNMATERIALIZE_BI: {
            /* We turn the instruction into a set that writes the unboxed big
             * integer value into the new target register. */
            MVMSTable *st = t->unmat_bi.st;
            MVMSpeshPEAAllocation *alloc = t->allocation;
            MVMSpeshIns *ins = t->unmat_bi.ins;
            allocate_concrete_registers(tc, g, gs, alloc);
            ins->info = MVM_op_get_op(MVM_OP_set);
            ins->operands[0].reg.orig = gs->attr_regs[alloc->hypothetical_attr_reg_idxs[0]];
            ins->operands[0] = MVM_spesh_manipulate_new_version(tc, g,
                ins->operands[0].reg.orig);
            ins->operands[1] = ins->operands[4];
            MVM_spesh_get_facts(tc, g, ins->operands[0])->writer = ins;
            pea_log("OPT: undone big integer materialization of %s into r%d(%d)",
                    st->debug_name, ins->operands[0].reg.orig,
                    ins->operands[0].reg.i);
            break;
        }
        case TRANSFORM_DECOMPOSE_BIGINT_REL: {
            /* Prepend instructions to read big integer out of box if needed. */
            MVMSpeshOperand a, b;
            MVMSpeshIns *ins = t->decomp_rel_bi.ins;
            if (t->decomp_rel_bi.dep_a && !t->decomp_rel_bi.dep_a->irreplaceable) {
                a.reg.orig = gs->attr_regs[t->decomp_rel_bi.hypothetical_reg_idx_a];
                a.reg.i = MVM_spesh_manipulate_get_current_version(tc, g, a.reg.orig);
            }
            else {
                MVMuint32 hyp_reg_idx = t->decomp_rel_bi.hypothetical_reg_idx_a;
                MVMSpeshIns *get_ins = MVM_spesh_alloc(tc, g, sizeof(MVMSpeshIns));
                get_ins->info = MVM_op_get_op(MVM_OP_sp_get_bi);
                get_ins->operands = MVM_spesh_alloc(tc, g, 3 * sizeof(MVMSpeshOperand));
                gs->attr_regs[hyp_reg_idx] = MVM_spesh_manipulate_get_unique_reg(tc, g, MVM_reg_rbi);
                a = get_ins->operands[0] = MVM_spesh_manipulate_new_version(tc, g,
                        gs->attr_regs[hyp_reg_idx]);
                get_ins->operands[1] = ins->operands[1];
                get_ins->operands[2].lit_ui16 = t->decomp_rel_bi.obtain_offset_a;
                MVM_spesh_get_facts(tc, g, get_ins->operands[0])->writer = get_ins;
                MVM_spesh_usages_add_by_reg(tc, g, get_ins->operands[1], get_ins);
                MVM_spesh_manipulate_insert_ins(tc, bb, ins->prev, get_ins);
            }
            if (t->decomp_rel_bi.dep_b && !t->decomp_rel_bi.dep_b->irreplaceable) {
                b.reg.orig = gs->attr_regs[t->decomp_rel_bi.hypothetical_reg_idx_b];
                b.reg.i = MVM_spesh_manipulate_get_current_version(tc, g, b.reg.orig);
            }
            else {
                MVMuint32 hyp_reg_idx = t->decomp_rel_bi.hypothetical_reg_idx_b;
                MVMSpeshIns *get_ins = MVM_spesh_alloc(tc, g, sizeof(MVMSpeshIns));
                get_ins->info = MVM_op_get_op(MVM_OP_sp_get_bi);
                get_ins->operands = MVM_spesh_alloc(tc, g, 3 * sizeof(MVMSpeshOperand));
                gs->attr_regs[hyp_reg_idx] = MVM_spesh_manipulate_get_unique_reg(tc, g, MVM_reg_rbi);
                b = get_ins->operands[0] = MVM_spesh_manipulate_new_version(tc, g,
                        gs->attr_regs[hyp_reg_idx]);
                get_ins->operands[1] = ins->operands[2];
                get_ins->operands[2].lit_ui16 = t->decomp_rel_bi.obtain_offset_b;
                MVM_spesh_get_facts(tc, g, get_ins->operands[0])->writer = get_ins;
                MVM_spesh_usages_add_by_reg(tc, g, get_ins->operands[1], get_ins);
                MVM_spesh_manipulate_insert_ins(tc, bb, ins->prev, get_ins);
            }

            /* Now, transform the instruction itself. */
            pea_log("OPT: big integer relational op %s devirtualized", ins->info->name);
            MVM_spesh_usages_delete_by_reg(tc, g, ins->operands[1], ins);
            MVM_spesh_usages_delete_by_reg(tc, g, ins->operands[2], ins);
            ins->info = MVM_op_get_op(t->decomp_rel_bi.replace_op);
            ins->operands[1] = a;
            ins->operands[2] = b;
            MVM_spesh_usages_add_by_reg(tc, g, ins->operands[1], ins);
            MVM_spesh_usages_add_by_reg(tc, g, ins->operands[2], ins);
            MVM_spesh_graph_add_comment(tc, g, ins, "big integer relational devirtualized");
            break;
        }
        default:
            MVM_oops(tc, "Unimplemented partial escape analysis transform");
    }
}

/* Adds a register to the set of those being tracked by the escape algorithm. */
static void add_tracked_register(MVMThreadContext *tc, GraphState *gs, MVMSpeshOperand reg,
                                 MVMSpeshPEAAllocation *alloc) {
    TrackedRegister tr;
    tr.reg = reg;
    tr.allocation = alloc;
    MVM_VECTOR_PUSH(gs->tracked_registers, tr);
}

/* Marks an allocation has having been seen. */
static void mark_allocation_seen(MVMThreadContext *tc, GraphState *gs, MVMSpeshBB *bb,
        MVMSpeshPEAAllocation *alloc) {
    BBState *bb_state = &(gs->bb_states[bb->idx]);
    BBAllocationState *a_state;
    MVM_VECTOR_ENSURE_SIZE(bb_state->alloc_state, alloc->index + 1);
    bb_state->alloc_state[alloc->index].seen = 1;
}

/* Sees if this is something we can potentially avoid really allocating. If
 * it is, sets up the allocation tracking state that we need. */
static MVMSpeshPEAAllocation * try_track_allocation(MVMThreadContext *tc, MVMSpeshGraph *g,
        GraphState *gs, MVMSpeshBB *alloc_bb, MVMSpeshIns *alloc_ins, MVMSTable *st) {
    if (st->REPR->ID == MVM_REPR_ID_P6opaque) {
        /* Go over the attributes, making sure we can handle them and allocating
         * a hypothetical register index for each of them. Bail if we cannot
         * handle them. */
        MVMP6opaqueREPRData *repr_data = (MVMP6opaqueREPRData *)st->REPR_data;
        MVMSpeshPEAAllocation *alloc = MVM_spesh_alloc(tc, g, sizeof(MVMSpeshPEAAllocation));
        MVMuint32 i;
        alloc->hypothetical_attr_reg_idxs = MVM_spesh_alloc(tc, g,
                repr_data->num_attributes * sizeof(MVMuint16));
        for (i = 0; i < repr_data->num_attributes; i++) {
            /* Make sure it's an attribute type we know how to handle. */
            MVMint32 kind = flattened_type_to_register_kind(tc, repr_data->flattened_stables[i]);
            if (kind < 0)
                return NULL;

            /* Pick an index that will later come to refer to an allocated
             * register if we apply transforms. */
            alloc->hypothetical_attr_reg_idxs[i] = gs->latest_hypothetical_reg_idx++;

            /* Note if it's a big integer boxing; we use this as part of the
             * heuristics for if we're doing a worthwhile rewrite. */
            if (kind == MVM_reg_obi)
                alloc->bigint = 1;
        }

        /* If we get here, we're going to track this allocation and try to do
         * scalar replacement of it. Set it up and store it. */
        alloc->allocator = alloc_ins;
        alloc->allocator_bb = alloc_bb;
        alloc->type = st->WHAT;
        alloc->index = MVM_VECTOR_ELEMS(gs->tracked_allocations);
        MVM_VECTOR_PUSH(gs->tracked_allocations, alloc);
        add_tracked_register(tc, gs, alloc_ins->operands[0], alloc);
        mark_allocation_seen(tc, gs, alloc_bb, alloc);
        return alloc;
    }
    return NULL;
}

/* Add a transform to hypothetically be applied. */
static void add_transform_for_bb(MVMThreadContext *tc, GraphState *gs, MVMSpeshBB *bb,
        Transformation *tran) {
    MVM_VECTOR_PUSH(gs->bb_states[bb->idx].transformations, tran);
}

/* Gets the shadow facts for a register, or returns NULL if there aren't
 * any. The _h form takes a hypothetical register ID, the _c form a
 * concrete register.*/
static MVMSpeshFacts * get_shadow_facts_h(MVMThreadContext *tc, GraphState *gs, MVMuint16 idx) {
    MVMint32 i;
    for (i = 0; i < gs->shadow_facts_num; i++) {
        ShadowFact *sf = &(gs->shadow_facts[i]);
        if (sf->is_hypothetical && sf->hypothetical_reg_idx == idx)
            return &(sf->facts);
    }
    return NULL;
}
static MVMSpeshFacts * get_shadow_facts_c(MVMThreadContext *tc, GraphState *gs, MVMSpeshOperand o) {
    MVMint32 i;
    for (i = 0; i < gs->shadow_facts_num; i++) {
        ShadowFact *sf = &(gs->shadow_facts[i]);
        if (!sf->is_hypothetical && sf->concrete_orig == o.reg.orig &&
                sf->concrete_i == o.reg.i)
            return &(sf->facts);
    }
    return NULL;
}

/* Shadow facts are facts that we hold about a value based upon the new
 * information we have available thanks to scalar replacement. This adds
 * a new one. Note that any previously held shadow facts are this point
 * may be invalidated due to reallocation. This will get recreate new
 * shadow facts if they already exist. The _h form takes a hypothetical
 * register ID, the _c form a concrete register. */
static MVMSpeshFacts * create_shadow_facts_h(MVMThreadContext *tc, GraphState *gs, MVMuint16 idx) {
    MVMSpeshFacts *facts = get_shadow_facts_h(tc, gs, idx);
    if (!facts) {
        ShadowFact sf;
        sf.is_hypothetical = 1;
        sf.hypothetical_reg_idx = idx;
        memset(&(sf.facts), 0, sizeof(MVMSpeshFacts));
        MVM_VECTOR_PUSH(gs->shadow_facts, sf);
        facts = &(gs->shadow_facts[gs->shadow_facts_num - 1].facts);
    }
    return facts;
}
static MVMSpeshFacts * create_shadow_facts_c(MVMThreadContext *tc, GraphState *gs, MVMSpeshOperand o) {
    MVMSpeshFacts *facts = get_shadow_facts_c(tc, gs, o);
    if (!facts) {
        ShadowFact sf;
        sf.is_hypothetical = 0;
        sf.concrete_orig = o.reg.orig;
        sf.concrete_i = o.reg.i;
        memset(&(sf.facts), 0, sizeof(MVMSpeshFacts));
        MVM_VECTOR_PUSH(gs->shadow_facts, sf);
        facts = &(gs->shadow_facts[gs->shadow_facts_num - 1].facts);
    }
    return facts;
}

/* Map an object offset to the register with its scalar replacement. */
static MVMuint16 attribute_offset_to_reg(MVMThreadContext *tc, MVMSpeshPEAAllocation *alloc,
        MVMint16 offset) {
    MVMuint32 idx = MVM_p6opaque_offset_to_attr_idx(tc, alloc->type, offset);
    return alloc->hypothetical_attr_reg_idxs[idx];
}

/* Check if an allocation is being tracked. */
static MVMuint32 allocation_tracked(MVMThreadContext *tc, GraphState *gs, MVMSpeshBB *bb,
                                    MVMSpeshPEAAllocation *alloc) {
    /* Must have an allocation record, must not be marked irreplaceable, and
     * must not have been materialized already. */
    if (alloc && !alloc->irreplaceable) {
        BBState *bb_state = &(gs->bb_states[bb->idx]);
        MVMint32 index = alloc->index;
        return index >= MVM_VECTOR_ALLOCATED(bb_state->alloc_state) ||
            MVM_VECTOR_ELEMS(bb_state->alloc_state[index].materializations) == 0;
    }
    return 0;
}

/* Gets the number of attributes in a tracked allocation. */
static MVMint32 get_num_attributes(MVMThreadContext *tc, MVMSpeshPEAAllocation *alloc) {
    MVMP6opaqueREPRData *repr_data = (MVMP6opaqueREPRData *)alloc->type->st->REPR_data;
    return repr_data->num_attributes;
}

/* Gets or allocates the used state for a tracked allocation in the current BB. */
static MVMuint8 * get_used_state(MVMThreadContext *tc, GraphState *gs, MVMSpeshBB *bb,
        MVMSpeshPEAAllocation *alloc) {
    BBState *bb_state = &(gs->bb_states[bb->idx]);
    BBAllocationState *a_state;
    MVM_VECTOR_ENSURE_SIZE(bb_state->alloc_state, alloc->index + 1);
    a_state = &(bb_state->alloc_state[alloc->index]);
    if (!a_state->used)
        a_state->used = MVM_calloc(1, get_num_attributes(tc, alloc));
    return a_state->used;
}

/* Marks an attribute in a tracked object as having been written. */
static void mark_attribute_written(MVMThreadContext *tc, GraphState *gs, MVMSpeshBB *bb,
        MVMSpeshPEAAllocation *alloc, MVMint16 offset) {
    MVMuint32 idx = MVM_p6opaque_offset_to_attr_idx(tc, alloc->type, offset);
    get_used_state(tc, gs, bb, alloc)[idx] = 1;
}

/* Checks if an attribute was written. */
static MVMint32 was_attribute_written(MVMThreadContext *tc, GraphState *gs, MVMSpeshBB *bb,
        MVMSpeshPEAAllocation *alloc, MVMint16 offset) {
    MVMuint32 idx = MVM_p6opaque_offset_to_attr_idx(tc, alloc->type, offset);
    return get_used_state(tc, gs, bb, alloc)[idx];
}

/* Adds a register to the target list of a materialization (that is, the registers
 * that we should write a materialization into). */
static void add_materialization_target_c(MVMThreadContext *tc, MVMSpeshGraph *g,
        Transformation *t, MVMSpeshOperand o) {
    MaterializationTarget *target = MVM_spesh_alloc(tc, g, sizeof(MaterializationTarget));
    target->reg = o;
    target->is_hypothetical = 0;
    target->next = t->materialize.targets;
    t->materialize.targets = target;
}
static void add_materialization_target_h(MVMThreadContext *tc, MVMSpeshGraph *g,
        Transformation *t, MVMuint16 hyp_reg) {
    MaterializationTarget *target = MVM_spesh_alloc(tc, g, sizeof(MaterializationTarget));
    target->hyp_reg = hyp_reg;
    target->is_hypothetical = 1;
    target->next = t->materialize.targets;
    t->materialize.targets = target;
}

/* Checks an instruction for use of materialized objects, and registers the
 * usage. */
static void add_materialization_target_if_missing(MVMThreadContext *tc, MVMSpeshGraph *g,
        Transformation *tran, MVMSpeshOperand user) {
    /* See if we already have the target register on the list; do nothing if
     * so. */
    MaterializationTarget *target = tran->materialize.targets;
    while (target) {
        if (!target->is_hypothetical && target->reg.reg.orig == user.reg.orig &&
                target->reg.reg.i == user.reg.i)
            return;
        target = target->next;
    }

    /* Otherwise, we need to add the target. */
    add_materialization_target_c(tc, g, tran, user);
}
static void handle_materialized_usages(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb,
        MVMSpeshIns *ins, GraphState *gs) {
   MVMuint32 i, j;
   for (i = 0; i < ins->info->num_operands; i++) {
       if ((ins->info->operands[i] & MVM_operand_rw_mask) == MVM_operand_read_reg) {
            MVMSpeshOperand user = ins->operands[i];
            MVMSpeshFacts *facts = MVM_spesh_get_facts(tc, g, user);
            MVMSpeshPEAAllocation *alloc = facts->pea.allocation;
            if (alloc && !alloc->irreplaceable) {
                BBState *bb_state = &(gs->bb_states[bb->idx]);
                MVMint32 index = alloc->index;
                if (index < MVM_VECTOR_ALLOCATED(bb_state->alloc_state)) {
                    BBAllocationState *a_state = &(bb_state->alloc_state[index]);
                    for (j = 0; j < MVM_VECTOR_ELEMS(a_state->materializations); j++)
                        add_materialization_target_if_missing(tc, g,
                                a_state->materializations[j], user);
                }
            }
       }
   }
}

/* Indicates that a real object is required. In most cases, we can insert a
 * materialization, though in others we must mark the object irreplaceable. */
static void mark_irreplaceable(MVMThreadContext *tc, MVMSpeshPEAAllocation *alloc) {
    alloc->irreplaceable = 1;
    while (MVM_VECTOR_ELEMS(alloc->escape_dependencies) > 0) {
        MVMSpeshPEAAllocation *nested = MVM_VECTOR_POP(alloc->escape_dependencies);
        pea_log("transitively marked another object escaped");
        mark_irreplaceable(tc, nested);
    }
}
static MVMint32 in_branch(MVMThreadContext *tc, GraphState *gs, MVMSpeshGraph *g,
        MVMSpeshBB *base, MVMSpeshBB *check) {
    /* Walk the graph in reverse postorder. When we visit a node with more than
     * one succ, add the extra succs on (entering a branch). When we visit a
     * node with more than one pred, add the extra preds on. When we find the
     * node to check, we expect to have a non-zero branch depth. */
    MVMint32 branch_depth = 0;
    MVMint32 i = base->rpo_idx;
    while (i < g->num_bbs) {
        MVMSpeshBB *cur = gs->rpo[i];
        if (cur != base)
            branch_depth -= cur->num_pred - 1;
        if (cur == check)
            return branch_depth != 0;
        branch_depth += cur->num_succ - 1;
        i++;
    }
    return 1; /* Not found; complex enough topology, so suppose branch. */
}
static MVMint32 worth_materializing(MVMThreadContext *tc, GraphState *gs, MVMSpeshGraph *g,
        MVMSpeshBB *bb, MVMSpeshIns *ins, MVMSpeshPEAAllocation *alloc) {
    /* It's worth materializing this if either:
     * 1. We read from the object (in which case we can have reduced costs
     *    in guards or indirections between the allocation and here)
     * 2. It is boxing a big integer, in which case the devirtualization of
     *    the big integer operation makes it worthwhile.
     * 3. We are materializing it in a branch. */
    return alloc->read || alloc->bigint ||
        in_branch(tc, gs, g, alloc->allocator_bb, bb);
}
static void materialize_attributes(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb,
                                   MVMSpeshIns *prior_ins, GraphState *gs,
                                   MVMSpeshPEAAllocation *obj_alloc) {
    /* Go through the attributes and see if any reference tracked objects. */
    MVMP6opaqueREPRData *repr_data = (MVMP6opaqueREPRData *)obj_alloc->type->st->REPR_data;
    MVMint32 i;
    for (i = 0; i < repr_data->num_attributes; i++) {
        MVMuint16 hypothetical_reg = obj_alloc->hypothetical_attr_reg_idxs[i];
        MVMSpeshFacts *attr_facts = get_shadow_facts_h(tc, gs, hypothetical_reg);
        if (attr_facts && allocation_tracked(tc, gs, bb, attr_facts->pea.allocation)) {
            /* Create the materialization transform. */
            MVMSpeshPEAAllocation *attr_alloc = attr_facts->pea.allocation;
            BBState *bb_state = &(gs->bb_states[bb->idx]);
            MVMint32 index = attr_alloc->index;
            Transformation *tran = MVM_spesh_alloc(tc, g, sizeof(Transformation));
            tran->allocation = attr_alloc;
            tran->transform = TRANSFORM_MATERIALIZE;
            tran->materialize.prior_to = prior_ins;
            tran->materialize.used = get_used_state(tc, gs, bb, attr_alloc);

            /* Add the hypothetical register of the attribute as a materialization
             * target. */
            add_materialization_target_h(tc, g, tran, hypothetical_reg);

            /* Record the materialization. */
            MVM_VECTOR_ENSURE_SIZE(bb_state->alloc_state, index + 1);
            MVM_VECTOR_PUSH(bb_state->alloc_state[index].materializations, tran);
            pea_log("inserting materialization of %s (%d) since enclosing %s is materialized",
                attr_alloc->type->st->debug_name, index, obj_alloc->type->st->debug_name);

            /* Repeat this process recursively. */
            materialize_attributes(tc, g, bb, prior_ins, gs, attr_alloc);
            add_transform_for_bb(tc, gs, bb, tran);
        }
    }
}
static void real_object_required(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb,
                                 MVMSpeshIns *ins, MVMSpeshOperand o, GraphState *gs,
                                 MVMint32 can_materialize) {
    /* Make sure we didn't already mark the object irreplaceable or materialize it. */
    MVMSpeshFacts *target = MVM_spesh_get_facts(tc, g, o);
    MVMSpeshPEAAllocation *alloc = target->pea.allocation;
    if (allocation_tracked(tc, gs, bb, alloc)) {
        int worthwhile = can_materialize ? worth_materializing(tc, gs, g, bb, ins, alloc) : 0;
        if (can_materialize && worthwhile) {
            /* Create the materialization transform. */
            BBState *bb_state = &(gs->bb_states[bb->idx]);
            MVMint32 index = alloc->index;
            Transformation *tran = MVM_spesh_alloc(tc, g, sizeof(Transformation));
            tran->allocation = alloc;
            tran->transform = TRANSFORM_MATERIALIZE;
            tran->materialize.prior_to = ins;
            tran->materialize.used = get_used_state(tc, gs, bb, alloc);

            /* Add the consuming register as a materialization target. */
            add_materialization_target_c(tc, g, tran, o);

            /* Record the materialization. */
            MVM_VECTOR_ENSURE_SIZE(bb_state->alloc_state, index + 1);
            MVM_VECTOR_PUSH(bb_state->alloc_state[index].materializations, tran);
            pea_log("inserting materialization of %s (%d) due to %s",
                    alloc->type->st->debug_name, index, ins->info->name);

            /* Make sure that we add materializations of any objects that
             * this one references, but are also tracked, too. */
            materialize_attributes(tc, g, bb, ins, gs, alloc);
            add_transform_for_bb(tc, gs, bb, tran);
        }
        else {
            pea_log(can_materialize && !worthwhile
                    ? "could replace and materialize a %s at %s, but not worthwhile"
                    : "replacement of %s impossible due to %s use of r%d(%d)",
                    alloc->type->st->debug_name, ins->info->name,
                    o.reg.orig, o.reg.i);
            mark_irreplaceable(tc, alloc);
        }
    }
}

/* Unhandled instructions cause anything they read to be materialized. */
static void unhandled_instruction(MVMThreadContext *tc, MVMSpeshGraph *g,
        MVMSpeshBB *bb, MVMSpeshIns *ins, GraphState *gs) {
   MVMuint32 i = 0;
   for (i = 0; i < ins->info->num_operands; i++)
       if ((ins->info->operands[i] & MVM_operand_rw_mask) == MVM_operand_read_reg)
            real_object_required(tc, g, bb, ins, ins->operands[i], gs, 1);
}

/* Takes a binary big integer operation, calculates how it could be decomposed
 * into big integer register ops, and adds a transform to do so. Forms for both
 * binary and unary ops that result in a new big integer. */
static MVMint32 are_types_known(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshIns *ins,
        MVMint32 from, MVMint32 to) {
    MVMint32 i;
    for (i = from; i <= to; i++) {
        MVMSpeshFacts *facts = MVM_spesh_get_facts(tc, g, ins->operands[i]);
        if (facts->flags & MVM_SPESH_FACT_KNOWN_TYPE) {
            if (REPR(facts->type)->ID == MVM_REPR_ID_P6opaque) {
                MVMuint16 offset = MVM_p6opaque_get_bigint_offset(tc, facts->type->st);
                if (!offset) {
                    pea_log("cannot decompose %s because the big integer offset cannot be found",
                            ins->info->name);
                    return 0;
                }
            }
            else {
                pea_log("cannot decompose operand to %s because it is not a P6opaque",
                        ins->info->name);
                return 0;
            }
        }
        else {
            pea_log("cannot decompose %s due to missing operand %d type information",
                    ins->info->name, i);
            return 0;
        }
    }
    return 1;
}
static int decompose_and_track_bigint_bi(MVMThreadContext *tc, MVMSpeshGraph *g,
        GraphState *gs, MVMSpeshBB *bb, MVMSpeshIns *ins, MVMuint16 replace_op) {
    MVMSTable *st;
    MVMSpeshPEAAllocation *alloc;

    /* Make sure that we know the types of the incoming operands and the result,
     * and we can resolve the big integer offset. */
    if (!are_types_known(tc, g, ins, 1, 3)) {
        unhandled_instruction(tc, g, bb, ins, gs);
        return 0;
    }

    /* See if we can track the result type. */
    st = MVM_spesh_get_facts(tc, g, ins->operands[3])->type->st;
    alloc = try_track_allocation(tc, g, gs, bb, ins, st);
    if (alloc) {
        /* Obtain tracked status of the incoming arguments. */
        MVMSpeshFacts *a_facts = MVM_spesh_get_facts(tc, g, ins->operands[1]);
        MVMSpeshPEAAllocation *a_alloc = a_facts->pea.allocation;
        MVMSpeshFacts *b_facts = MVM_spesh_get_facts(tc, g, ins->operands[2]);
        MVMSpeshPEAAllocation *b_alloc = b_facts->pea.allocation;

        /* Assemble a decompose transform. If the incoming arguments are
         * tracked, then we just will use the hypothetical register of the
         * tracked object's big integer slot. Otherwise, we will allocate a
         * hypothetical register to read it into. */
        Transformation *tran = MVM_spesh_alloc(tc, g, sizeof(Transformation));
        tran->allocation = alloc;
        tran->transform = TRANSFORM_DECOMPOSE_BIGINT_BI;
        tran->decomp_op_bi.ins = ins;
        if (allocation_tracked(tc, gs, bb, a_alloc)) {
            /* Find the hypothetical register for the attribute in question.
             * Also, add a dependency on the allocation in question being
             * replaced. */ 
            tran->decomp_op_bi.hypothetical_reg_idx_a = find_bigint_register(tc, a_alloc);
            MVM_VECTOR_PUSH(alloc->escape_dependencies, a_alloc);
            a_alloc->read = 1;
        }
        else {
            /* Allocate a hypothetical big integer reference register, which
             * we read the value into, and store the offset to read from (which
             * is our indication that we need to read out of the object too). */
            tran->decomp_op_bi.hypothetical_reg_idx_a = gs->latest_hypothetical_reg_idx++;
            tran->decomp_op_bi.obtain_offset_a = MVM_p6opaque_get_bigint_offset(tc,
                    a_facts->type->st);
        }
        if (allocation_tracked(tc, gs, bb, b_alloc)) {
            tran->decomp_op_bi.hypothetical_reg_idx_b = find_bigint_register(tc, b_alloc);
            MVM_VECTOR_PUSH(alloc->escape_dependencies, b_alloc);
            b_alloc->read = 1;
        }
        else {
            tran->decomp_op_bi.hypothetical_reg_idx_b = gs->latest_hypothetical_reg_idx++;
            tran->decomp_op_bi.obtain_offset_b = MVM_p6opaque_get_bigint_offset(tc,
                    b_facts->type->st);
        }
        tran->decomp_op_bi.replace_op = replace_op;
        add_transform_for_bb(tc, gs, bb, tran);
        MVM_spesh_get_facts(tc, g, ins->operands[0])->pea.allocation = alloc;
        mark_attribute_written(tc, gs, bb, alloc,
                MVM_p6opaque_get_bigint_offset(tc, alloc->type->st) - sizeof(MVMObject));
        pea_log("started tracking a big integer allocation");

        /* Mark all facts as used. */
        MVM_spesh_use_facts(tc, g, a_facts);
        MVM_spesh_use_facts(tc, g, b_facts);
        MVM_spesh_use_facts(tc, g, MVM_spesh_get_facts(tc, g, ins->operands[3]));

        return 1;
    }
    else {
        unhandled_instruction(tc, g, bb, ins, gs);
        return 0;
    }
}
static int decompose_and_track_bigint_un(MVMThreadContext *tc, MVMSpeshGraph *g,
        GraphState *gs, MVMSpeshBB *bb, MVMSpeshIns *ins, MVMuint16 replace_op) {
    MVMSTable *st;
    MVMSpeshPEAAllocation *alloc;

    /* Make sure that we know the types of the incoming operand and the result,
     * and we can resolve the big integer offset. */
    if (!are_types_known(tc, g, ins, 1, 2)) {
        unhandled_instruction(tc, g, bb, ins, gs);
        return 0;
    }

    /* See if we can track the result type. */
    st = MVM_spesh_get_facts(tc, g, ins->operands[2])->type->st;
    alloc = try_track_allocation(tc, g, gs, bb, ins, st);
    if (alloc) {
        /* Obtain tracked status of the incoming operand. */
        MVMSpeshFacts *a_facts = MVM_spesh_get_facts(tc, g, ins->operands[1]);
        MVMSpeshPEAAllocation *a_alloc = a_facts->pea.allocation;

        /* Assemble a decompose transform. If the incoming argument is
         * tracked, then we just will use the hypothetical register of the
         * tracked object's big integer slot. Otherwise, we will allocate a
         * hypothetical register to read it into. */
        Transformation *tran = MVM_spesh_alloc(tc, g, sizeof(Transformation));
        tran->allocation = alloc;
        tran->transform = TRANSFORM_DECOMPOSE_BIGINT_UN;
        tran->decomp_op_bi.ins = ins;
        if (allocation_tracked(tc, gs, bb, a_alloc)) {
            tran->decomp_op_bi.hypothetical_reg_idx_a = find_bigint_register(tc, a_alloc);
            MVM_VECTOR_PUSH(alloc->escape_dependencies, a_alloc);
            a_alloc->read = 1;
        }
        else {
            tran->decomp_op_bi.hypothetical_reg_idx_a = gs->latest_hypothetical_reg_idx++;
            tran->decomp_op_bi.obtain_offset_a = MVM_p6opaque_get_bigint_offset(tc,
                    a_facts->type->st);
        }
        tran->decomp_op_bi.replace_op = replace_op;
        add_transform_for_bb(tc, gs, bb, tran);
        MVM_spesh_get_facts(tc, g, ins->operands[0])->pea.allocation = alloc;
        mark_attribute_written(tc, gs, bb, alloc,
                MVM_p6opaque_get_bigint_offset(tc, alloc->type->st) - sizeof(MVMObject));
        pea_log("started tracking a big integer allocation");

        /* Mark all facts as used. */
        MVM_spesh_use_facts(tc, g, a_facts);
        MVM_spesh_use_facts(tc, g, MVM_spesh_get_facts(tc, g, ins->operands[2]));

        return 1;
    }
    else {
        unhandled_instruction(tc, g, bb, ins, gs);
        return 0;
    }
}

/* Takes a big integer relational op and tries to decompose it, so we can either
 * use an already unboxed input argument, or have cheaper access to it. */
static int decompose_bigint_relational(MVMThreadContext *tc, MVMSpeshGraph *g,
        GraphState *gs, MVMSpeshBB *bb, MVMSpeshIns *ins, MVMuint16 replace_op) {
    /* Make sure that we know the types of the incoming operands, and we can
     * resolve the big integer offset. */
    if (are_types_known(tc, g, ins, 1, 2)) {
        /* Obtain tracked status of the incoming arguments. */
        MVMSpeshFacts *a_facts = MVM_spesh_get_facts(tc, g, ins->operands[1]);
        MVMSpeshPEAAllocation *a_alloc = a_facts->pea.allocation;
        MVMSpeshFacts *b_facts = MVM_spesh_get_facts(tc, g, ins->operands[2]);
        MVMSpeshPEAAllocation *b_alloc = b_facts->pea.allocation;

        /* Assemble a decompose transform for the relational op. This is a bit
         * of an unusual transform in that it does not belong to any particular
         * allocation, but its exact behavior (use hypothetical register vs.
         * emit a decomposition) will depend on what we end up deciding with
         * regards to escape/replceability. Thus even in the case where we store
         * the hypothetical register for if it is scalar replaced, we also
         * store the offset so we can fall back on a read from the object. */
        Transformation *tran = MVM_spesh_alloc(tc, g, sizeof(Transformation));
        tran->transform = TRANSFORM_DECOMPOSE_BIGINT_REL;
        tran->decomp_rel_bi.ins = ins;
        tran->decomp_rel_bi.replace_op = replace_op;
        if (allocation_tracked(tc, gs, bb, a_alloc)) {
            tran->decomp_rel_bi.hypothetical_reg_idx_a = find_bigint_register(tc, a_alloc);
            tran->decomp_rel_bi.dep_a = a_alloc;
            a_alloc->read = 1;
        }
        else {
            tran->decomp_rel_bi.hypothetical_reg_idx_a = gs->latest_hypothetical_reg_idx++;
        }
        tran->decomp_rel_bi.obtain_offset_a = MVM_p6opaque_get_bigint_offset(tc,
                a_facts->type->st);
        if (allocation_tracked(tc, gs, bb, b_alloc)) {
            tran->decomp_rel_bi.hypothetical_reg_idx_b = find_bigint_register(tc, b_alloc);
            tran->decomp_rel_bi.dep_b = b_alloc;
            b_alloc->read = 1;
        }
        else {
            tran->decomp_rel_bi.hypothetical_reg_idx_b = gs->latest_hypothetical_reg_idx++;
        }
        tran->decomp_rel_bi.obtain_offset_b = MVM_p6opaque_get_bigint_offset(tc,
                b_facts->type->st);
        add_transform_for_bb(tc, gs, bb, tran);

        /* Mark all facts as used. */
        MVM_spesh_use_facts(tc, g, a_facts);
        MVM_spesh_use_facts(tc, g, b_facts);

        return 1;

    }
    else {
        unhandled_instruction(tc, g, bb, ins, gs);
        return 0;
    }
}

/* Tries to rewrite a decont_i on a tracked register into a use of a boxed value. */
static int try_replace_decont_i(MVMThreadContext *tc, MVMSpeshGraph *g, GraphState *gs, 
        MVMSpeshBB *bb, MVMSpeshIns *ins, MVMSpeshPEAAllocation *alloc) {
    MVMSTable *st = alloc->type->st;
    if (st->REPR->ID == MVM_REPR_ID_P6opaque) {
        MVMP6opaqueREPRData *repr_data = (MVMP6opaqueREPRData *)st->REPR_data;
        MVMuint32 i;
        for (i = 0; i < repr_data->num_attributes; i++) {
            MVMuint16 kind = flattened_type_to_register_kind(tc, repr_data->flattened_stables[i]);
            if (kind == MVM_reg_obi) {
                /* We can replace this with an unbox of a big integer register
                 * produced by scalar replacement. */
                Transformation *tran = MVM_spesh_alloc(tc, g, sizeof(Transformation));
                tran->allocation = alloc;
                tran->transform = TRANSFORM_UNBOX_BIGINT;
                tran->unbox_bi.ins = ins;
                tran->unbox_bi.hypothetical_reg_idx = alloc->hypothetical_attr_reg_idxs[i];
                add_transform_for_bb(tc, gs, bb, tran);
                alloc->read = 1;
                return 1;
            }
        }
    }
    return 0;
}

/* Checks if any of the tracked objects are needed beyond this deopt point,
 * and adds a transform to set up that deopt info if needed. Also makes sure
 * that current versions of registers used in scalar replacement will have a
 * deopt usage added, otherwise the data we need to deopt could go missing. */
static void add_scalar_replacement_deopt_usages(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb,
                                                GraphState *gs, MVMSpeshPEAAllocation *alloc,
                                                MVMint32 deopt_idx) {
    MVMP6opaqueREPRData *repr_data = (MVMP6opaqueREPRData *)alloc->type->st->REPR_data;
    MVMuint32 i;
    for (i = 0; i < repr_data->num_attributes; i++) {
        Transformation *tran = MVM_spesh_alloc(tc, g, sizeof(Transformation));
        tran->allocation = alloc;
        tran->transform = TRANSFORM_ADD_DEOPT_USAGE;
        tran->du.deopt_point_idx = deopt_idx;
        tran->du.hypothetical_reg_idx = alloc->hypothetical_attr_reg_idxs[i];
        add_transform_for_bb(tc, gs, bb, tran);
    }
}
static void add_deopt_materializations_idx(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb,
                                           GraphState *gs, MVMint32 deopt_idx,
                                           MVMint32 deopt_user_idx) {
    MVMint32 i;
    for (i = 0; i < MVM_VECTOR_ELEMS(gs->tracked_registers); i++) {
        MVMSpeshFacts *tracked_facts = MVM_spesh_get_facts(tc, g, gs->tracked_registers[i].reg);
        MVMSpeshPEAAllocation *alloc = tracked_facts->pea.allocation;
        if (allocation_tracked(tc, gs, bb, alloc)) {
            MVMSpeshDeoptUseEntry *deopt_user = tracked_facts->usage.deopt_users;
            while (deopt_user) {
                if (deopt_user->deopt_idx == deopt_user_idx) {
                    Transformation *tran = MVM_spesh_alloc(tc, g, sizeof(Transformation));
                    tran->allocation = alloc;
                    tran->transform = TRANSFORM_ADD_DEOPT_POINT;
                    tran->dp.deopt_point_idx = deopt_idx;
                    tran->dp.target_reg = gs->tracked_registers[i].reg.reg.orig;
                    add_transform_for_bb(tc, gs, bb, tran);
                    add_scalar_replacement_deopt_usages(tc, g, bb, gs, alloc, deopt_user_idx);
                }
                deopt_user = deopt_user->next;
            }
        }
    }
}

/* Goes through the deopt indices at the specified instruction, and sees if
 * any of the tracked objects are needed beyond the deopt point. If so,
 * adds their materialization. */
static void add_deopt_materializations_ins(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb,
                                           GraphState *gs, MVMSpeshIns *deopt_ins) {
    /* Make a first pass to see if there's a SYNTH deopt index; if there is,
     * that is the one we use to do a lookup inside of the usages. */
    MVMint32 deopt_user_idx = -1;
    MVMSpeshAnn *ann = deopt_ins->annotations;
    while (ann) {
        if (ann->type == MVM_SPESH_ANN_DEOPT_SYNTH) {
            deopt_user_idx = ann->data.deopt_idx;
            break;
        }
        ann = ann->next;
    }

    /* Now go over the concrete indexes that will appear when we actually deopt. */
    ann = deopt_ins->annotations;
    while (ann) {
        switch (ann->type) {
            case MVM_SPESH_ANN_DEOPT_ONE_INS:
            case MVM_SPESH_ANN_DEOPT_ALL_INS:
            case MVM_SPESH_ANN_DEOPT_INLINE:
                add_deopt_materializations_idx(tc, g, bb, gs, ann->data.deopt_idx,
                        deopt_user_idx >= 0 ? deopt_user_idx : ann->data.deopt_idx);
                break;
        }
        ann = ann->next;
    }
}

/* Go through the predecessor basic blocks, checking if allocations have been
 * materialized there, building up the initial allocation state for this basic
 * block. */
static void setup_bb_state(MVMThreadContext *tc, GraphState *gs, MVMSpeshBB *new_bb) {
    BBState *new_bb_state = &(gs->bb_states[new_bb->idx]);
    MVMint32 num_allocs = MVM_VECTOR_ELEMS(gs->tracked_allocations);
    MVMint32 i, j, k;
    MVM_VECTOR_INIT(new_bb_state->alloc_state, num_allocs);
    for (i = 0; i < num_allocs; i++) {
        /* Go through the predecessors and see if any of them have materialized
         * the object, as well as counting up how many preds have written to the
         * attribute. Build up a set of distinct materializations. */
        MVMint32 num_materialized = 0;
        MVMuint32 num_attrs = get_num_attributes(tc, gs->tracked_allocations[i]);
        MVMuint8 *new_used = MVM_calloc(1, num_attrs);
        MVMint32 consistent = 1;
        MVM_VECTOR_DECL(Transformation *, distinct_materializations);
        MVM_VECTOR_DECL(MVMSpeshBB *, applicable_bbs);
        MVM_VECTOR_INIT(distinct_materializations, 0);
        MVM_VECTOR_INIT(applicable_bbs, 0);
        for (j = 0; j < new_bb->num_pred; j++) {
            MVMSpeshBB *pred_bb = new_bb->pred[j];
            BBState *pred_bb_state = &(gs->bb_states[pred_bb->idx]);
            if (i < MVM_VECTOR_ALLOCATED(pred_bb_state->alloc_state) &&
                    pred_bb_state->alloc_state[i].seen) {
                BBAllocationState *a_state = &(pred_bb_state->alloc_state[i]);

                /* Merged used in preds. */
                MVMuint8 *pred_used = a_state->used;
                if (pred_used)
                    for (k = 0; k < num_attrs; k++)
                        new_used[k] += pred_used[k];

                /* Merge materializations lists (distinct entries only). */
                if (MVM_VECTOR_ELEMS(a_state->materializations) > 0) {
                    num_materialized++;
                    for (k = 0; k < MVM_VECTOR_ELEMS(a_state->materializations); k++) {
                        Transformation *t = a_state->materializations[k];
                        MVMint32 already_seen;
                        MVM_VECTOR_CONTAINS(distinct_materializations, t, already_seen);
                        if (!already_seen)
                            MVM_VECTOR_PUSH(distinct_materializations, t);
                    }
                }

                /* If we're here, we've seen this allocation in a previous BB. */
                new_bb_state->alloc_state[i].seen = 1;

                /* And this BB is applicable. */
                MVM_VECTOR_PUSH(applicable_bbs, pred_bb);
            }
        }

        /* Look for discrepancies in writes, bail out if they are inconsistent,
         * and normalize the values to 1 if written. */
        for (j = 0; j < num_attrs; j++) {
            if (new_used[j]) {
                if (new_used[j] == MVM_VECTOR_ELEMS(applicable_bbs)) {
                    /* Consistently written by all. */
                    new_used[j] = 1;
                }
                else {
                    /* Inconsistently written. */
                    pea_log("Inconsistently written attribute in %s; too complex to handle",
                            gs->tracked_allocations[i]->type->st->debug_name);
                    mark_irreplaceable(tc, gs->tracked_allocations[i]);
                    consistent = 0;
                    break;
                }
            }
        }
        if (!consistent) {
            MVM_VECTOR_DESTROY(distinct_materializations);
            MVM_VECTOR_DESTROY(applicable_bbs);
            continue;
        }

        /* Set materialization state in new BB state. */
        MVM_VECTOR_ASSIGN(new_bb_state->alloc_state[i].materializations, distinct_materializations);
        new_bb_state->alloc_state[i].used = new_used;

        /* If we have any materialized, and it's not equal to the number of
         * preds, then the object has only been materialized on some paths to
         * this point. We'll need to ensure it's materialized on all of them. */
        if (num_materialized > 0 && num_materialized != MVM_VECTOR_ELEMS(applicable_bbs)) {
            /* TODO Insert materialization transforms. For now, we will just
             * conservatively mark the object irreplaceable. */
            pea_log("Cannot yet handle differring materialization state in preds");
            mark_irreplaceable(tc, gs->tracked_allocations[i]);
        }

        MVM_VECTOR_DESTROY(applicable_bbs);
    }
}

/* Add a transform that turns an object read into an a register reader
 * (or, if that object is also tracked, potentially into nothing). */
static void add_object_read_transform(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb,
        MVMSpeshIns *ins, GraphState *gs, MVMSpeshPEAAllocation *alloc) {
    MVMuint16 opcode = ins->info->opcode;
    MVMint32 is_object_get = opcode == MVM_OP_sp_get_o ||
        opcode == MVM_OP_sp_getvc_o ||
        opcode == MVM_OP_sp_getvt_o ||
        opcode == MVM_OP_sp_p6oget_o ||
        opcode == MVM_OP_sp_p6ogetvc_o ||
        opcode == MVM_OP_sp_p6ogetvt_o;
    MVMint32 is_p6o_op = opcode != MVM_OP_sp_get_o &&
        opcode != MVM_OP_sp_getvc_o &&
        opcode != MVM_OP_sp_getvt_o &&
        opcode != MVM_OP_sp_get_i64 &&
        opcode != MVM_OP_sp_get_n &&
        opcode != MVM_OP_sp_get_s;
    MVMint32 offset = is_p6o_op
        ? ins->operands[2].lit_i16
        : ins->operands[2].lit_i16 - sizeof(MVMObject);
    MVMuint16 hypothetical_reg = attribute_offset_to_reg(tc, alloc, offset);
    Transformation *tran = MVM_spesh_alloc(tc, g, sizeof(Transformation));
    tran->allocation = alloc;
    tran->transform = TRANSFORM_GETATTR_TO_SET;
    tran->attr.ins = ins;
    tran->attr.hypothetical_reg_idx = hypothetical_reg;
    if (is_object_get) {
        /* We're reading an object out of an object that doesn't
         * escape. We may have carried some facts about that. */
        MVMSpeshFacts *src_facts = get_shadow_facts_h(tc, gs,
                hypothetical_reg);
        if (src_facts) {
            /* Copy the facts (need to re-read them, since src_facts is
             * an interior point that the create call below might
             * move). */
            MVMSpeshFacts *tgt_facts = create_shadow_facts_c(tc, gs,
                ins->operands[0]);
            src_facts = get_shadow_facts_h(tc, gs, hypothetical_reg);
            MVM_spesh_copy_facts_resolved(tc, g, tgt_facts, src_facts);
            tgt_facts->pea.depend_allocation = alloc;

            /* We might be reading an object that itself is perhaps
             * being scalar replaced. If so, then we note that in the
             * transform, since it may need to simply delete this
             * instruction. We also need to track the target register
             * of the attribute read, since it now aliases a scalar
             * replaced object. The allocation needs to go on the real
             * facts, not the shadow ones. */
            if (allocation_tracked(tc, gs, bb, src_facts->pea.allocation)) {
                MVMSpeshPEAAllocation *src_alloc = src_facts->pea.allocation;
                tran->attr.target_allocation = src_alloc;
                MVM_spesh_get_facts(tc, g, ins->operands[0])->pea.allocation = src_alloc;
                add_tracked_register(tc, gs, ins->operands[0], src_alloc);
            }
        }
    }
    add_transform_for_bb(tc, gs, bb, tran);
    alloc->read = 1;
}

/* Add a transform that turns an object initial access into a write of the
 * initial value. */
static void add_object_autoviv_transform(MVMThreadContext *tc, MVMSpeshGraph *g,
        MVMSpeshBB *bb, MVMSpeshIns *ins, GraphState *gs, MVMSpeshPEAAllocation *alloc,
        MVMint32 offset) {
    /* Work out various properties of the vivification. */
    MVMuint16 opcode = ins->info->opcode;
    MVMint32 is_concrete_viv = opcode == MVM_OP_sp_getvc_o || opcode == MVM_OP_sp_p6ogetvc_o;
    MVMuint16 hypothetical_reg = attribute_offset_to_reg(tc, alloc, offset);

    /* Work out the auto-viv type and build the appropriate transform. */
    Transformation *tran = MVM_spesh_alloc(tc, g, sizeof(Transformation));
    tran->allocation = alloc;
    tran->transform = is_concrete_viv ? TRANSFORM_VIVIFY_CONCRETE : TRANSFORM_VIVIFY_TYPE;
    tran->viv.ins = ins;
    tran->viv.hypothetical_reg_idx = hypothetical_reg;
    tran->viv.type_sslot = ins->operands[3].lit_i16;
    add_transform_for_bb(tc, gs, bb, tran);

    /* Mark attribute written, and mark object read. */
    mark_attribute_written(tc, gs, bb, alloc, offset);
    alloc->read = 1;
}

/* Performs the analysis phase of partial escape anslysis, figuring out what
 * rewrites we can do on the graph to achieve scalar replacement of objects
 * and, perhaps, some guard eliminations. */
static MVMuint32 analyze(MVMThreadContext *tc, MVMSpeshGraph *g, GraphState *gs) {
    MVMSpeshBB **rpo = MVM_spesh_graph_reverse_postorder(tc, g);
    MVMuint8 *seen = MVM_calloc(g->num_bbs, 1);
    MVMuint32 found_replaceable = 0;
    MVMuint32 ins_count = 0;
    MVMuint32 i;
    gs->rpo = rpo;
    for (i = 0; i < g->num_bbs; i++) {
        MVMSpeshBB *bb = rpo[i];
        MVMSpeshIns *ins = bb->first_ins;

        /* For now, we don't handle loops; bail entirely if we see one. */
        MVMuint32 j;
        for (j = 0; j < bb->num_pred; j++) {
            if (!seen[bb->pred[j]->rpo_idx]) {
                pea_log("partial escape analysis not implemented for loops");
                MVM_free(seen);
                MVM_free(rpo);
                return 0;
            }
        }

        /* Initialize per-BB allocation state based on our predecessors (the
         * above check means we can for now assume they all have that state).
         * This may insert materializations in our predecessors also. */
        setup_bb_state(tc, gs, bb);

        while (ins) {
            MVMuint16 opcode = ins->info->opcode;

            /* See if this is an instruction where a deopt might take place.
             * If yes, then we first consider whether it's a guard that the
             * extra information available thanks to Scalar Replacement might
             * let us eliminate. If it *is*, then we no longer consider this a
             * deopt point, and schedule a transform of the guard into a set.
             * Also, make entries into the deopt materializations table. */
            MVMuint32 settified_guard = 0;
            if (ins->info->may_cause_deopt) {
                MVMSpeshPEAAllocation *settify_dep = NULL;
                MVMSpeshPEAAllocation *settify_target = NULL;
                switch (opcode) {
                    case MVM_OP_sp_guardconc: {
                        MVMSpeshFacts *hyp_facts = get_shadow_facts_c(tc, gs,
                                ins->operands[1]);
                        if (hyp_facts && (hyp_facts->flags & MVM_SPESH_FACT_CONCRETE) &&
                                (hyp_facts->flags & MVM_SPESH_FACT_KNOWN_TYPE) &&
                                hyp_facts->pea.depend_allocation) {
                            MVMSTable *wanted = (MVMSTable *)g->spesh_slots[ins->operands[2].lit_ui16];
                            settified_guard = wanted == hyp_facts->type->st;
                            settify_dep = hyp_facts->pea.depend_allocation;
                            if (allocation_tracked(tc, gs, bb, hyp_facts->pea.allocation)) {
                                settify_target = hyp_facts->pea.allocation;
                            }
                            else {
                                MVMSpeshFacts *facts = MVM_spesh_get_facts(tc, g,
                                    ins->operands[1]);
                                if (allocation_tracked(tc, gs, bb, facts->pea.allocation))
                                    settify_target = facts->pea.allocation;
                            }
                        }
                        break;
                    }
                }
                if (settified_guard) {
                    Transformation *tran = MVM_spesh_alloc(tc, g, sizeof(Transformation));
                    tran->allocation = settify_dep;
                    tran->transform = TRANSFORM_GUARD_TO_SET;
                    tran->guard.ins = ins;
                    tran->guard.target_allocation = settify_target;
                    add_transform_for_bb(tc, gs, bb, tran);
                    settify_dep->read = 1;
                }
                add_deopt_materializations_ins(tc, g, bb, gs, ins);
            }

            /* If the instruction uses a materialized value, we may need to
             * record that usage, so the materialization happens and the
             * correct aliases are set up. */
            handle_materialized_usages(tc, g, bb, ins, gs);

            /* Look for significant instructions. */
            switch (opcode) {
                case MVM_OP_sp_fastcreate:
                case MVM_OP_sp_materialize_bi: {
                    MVMSTable *st = (MVMSTable *)g->spesh_slots[ins->operands[2].lit_i16];
                    MVMSpeshPEAAllocation *alloc = try_track_allocation(tc, g, gs, bb, ins, st);
                    if (alloc) {
                        MVMSpeshFacts *target = MVM_spesh_get_facts(tc, g, ins->operands[0]);
                        Transformation *tran = MVM_spesh_alloc(tc, g, sizeof(Transformation));
                        tran->allocation = alloc;
                        if (opcode == MVM_OP_sp_materialize_bi) {
                            /* This is a bigint materialization. It will write the value
                             * of the big integer. */
                            tran->transform = TRANSFORM_UNMATERIALIZE_BI;
                            tran->unmat_bi.ins = ins;
                            tran->unmat_bi.st = st;
                            tran->unmat_bi.unboxed = ins->operands[4];
                        }
                        else {
                            tran->transform = TRANSFORM_DELETE_FASTCREATE;
                            tran->fastcreate.ins = ins;
                            tran->fastcreate.st = st;
                        }
                        add_transform_for_bb(tc, gs, bb, tran);
                        target->pea.allocation = alloc;
                        found_replaceable = 1;
                    }
                    break;
                }
                case MVM_OP_set: {
                    /* A set instruction just aliases the tracked object; we
                     * can potentially elimiante it. */
                    MVMSpeshFacts *source = MVM_spesh_get_facts(tc, g, ins->operands[1]);
                    MVMSpeshPEAAllocation *alloc = source->pea.allocation;
                    if (allocation_tracked(tc, gs, bb, alloc)) {
                        /* Add a transform to delete the set instruction. */
                        Transformation *tran = MVM_spesh_alloc(tc, g, sizeof(Transformation));
                        tran->allocation = alloc;
                        tran->transform = TRANSFORM_DELETE_SET;
                        tran->set.ins = ins;
                        add_transform_for_bb(tc, gs, bb, tran);
                        MVM_spesh_get_facts(tc, g, ins->operands[0])->pea.allocation = alloc;
                        add_tracked_register(tc, gs, ins->operands[0], alloc);

                        /* Propagate facts; sometimes they're missing from earlier
                         * passes. */
                        MVM_spesh_copy_facts_resolved(tc, g,
                                MVM_spesh_get_facts(tc, g, ins->operands[0]),
                                source);
                    }
                    break;
                }
                case MVM_OP_sp_bind_i64:
                case MVM_OP_sp_bind_n:
                case MVM_OP_sp_bind_s:
                case MVM_OP_sp_bind_s_nowb:
                case MVM_OP_sp_bind_o:
                case MVM_OP_sp_bind_o_nowb:
                case MVM_OP_sp_p6obind_i:
                case MVM_OP_sp_p6obind_n:
                case MVM_OP_sp_p6obind_s:
                case MVM_OP_sp_p6obind_o: {
                    /* Schedule transform of bind into an attribute of a
                     * tracked object into a set. */
                    MVMSpeshFacts *target = MVM_spesh_get_facts(tc, g, ins->operands[0]);
                    MVMSpeshPEAAllocation *alloc = target->pea.allocation;
                    MVMint32 is_object_bind = opcode == MVM_OP_sp_p6obind_o || opcode == MVM_OP_sp_bind_o || opcode == MVM_OP_sp_bind_o_nowb;
                    if (allocation_tracked(tc, gs, bb, alloc)) {
                        MVMint32 is_p6o_op = opcode == MVM_OP_sp_p6obind_i ||
                            opcode == MVM_OP_sp_p6obind_n ||
                            opcode == MVM_OP_sp_p6obind_s ||
                            opcode == MVM_OP_sp_p6obind_o;
                        MVMint32 offset = is_p6o_op
                            ? ins->operands[1].lit_i16
                            : ins->operands[1].lit_i16 - sizeof(MVMObject);
                        MVMuint16 hypothetical_reg = attribute_offset_to_reg(tc, alloc, offset);
                        Transformation *tran = MVM_spesh_alloc(tc, g, sizeof(Transformation));
                        tran->allocation = alloc;
                        tran->transform = TRANSFORM_BINDATTR_TO_SET;
                        tran->attr.ins = ins;
                        tran->attr.hypothetical_reg_idx = hypothetical_reg;
                        if (is_object_bind) {
                            /* We're binding one object into another. Create shadow facts
                             * for the target register that we replace into. */
                            MVMSpeshFacts *tgt_facts = create_shadow_facts_h(tc, gs,
                                    hypothetical_reg);
                            MVMSpeshFacts *src_facts = MVM_spesh_get_facts(tc, g,
                                    ins->operands[2]);
                            MVM_spesh_copy_facts_resolved(tc, g, tgt_facts, src_facts);

                            /* Check if that target object is tracked too, in which case
                             * we can potentially not really do any assignment here. */
                            if (allocation_tracked(tc, gs, bb, src_facts->pea.allocation)) {
                                /* Mark transform as dependent on the source, so we'll
                                 * just do a delete of this instruction if it also ends
                                 * up not escaping. */
                                MVMSpeshPEAAllocation *src_alloc = src_facts->pea.allocation;
                                tran->attr.target_allocation = src_alloc;
                                tgt_facts->pea.allocation = src_alloc;

                                /* Record that the allocation we're binding escapes if
                                 * the thing it's being bound into escapes. */
                                MVM_VECTOR_PUSH(alloc->escape_dependencies, src_alloc);
                            }
                        }
                        add_transform_for_bb(tc, gs, bb, tran);
                        mark_attribute_written(tc, gs, bb, alloc, offset);
                    }
                    else {
                        /* The target of the bind escapes; if this is an object
                         * bind then the target escapes. */
                        if (is_object_bind)
                            real_object_required(tc, g, bb, ins, ins->operands[2], gs, 1);
                    }
                    break;
                }
                case MVM_OP_sp_getvc_o:
                case MVM_OP_sp_getvt_o:
                case MVM_OP_sp_p6ogetvc_o:
                case MVM_OP_sp_p6ogetvt_o: {
                    /* Vivifying reads. Check if we've written it; if not, we will
                     * need to turn this read into an initial bind. */
                    MVMSpeshFacts *target = MVM_spesh_get_facts(tc, g, ins->operands[1]);
                    MVMSpeshPEAAllocation *alloc = target->pea.allocation;
                    if (allocation_tracked(tc, gs, bb, alloc)) {
                        MVMint32 is_p6o_op = opcode == MVM_OP_sp_p6ogetvc_o ||
                            opcode == MVM_OP_sp_p6ogetvt_o;
                        MVMint32 offset = is_p6o_op
                            ? ins->operands[2].lit_i16
                            : ins->operands[2].lit_i16 - sizeof(MVMObject);
                        if (was_attribute_written(tc, gs, bb, alloc, offset))
                            /* Already written, so just a normal access. */
                            add_object_read_transform(tc, g, bb, ins, gs, alloc);
                        else
                            /* First read, so we need to initialize the attribute. */
                            add_object_autoviv_transform(tc, g, bb, ins, gs, alloc, offset);
                    }
                    break;
                }
                case MVM_OP_sp_get_o:
                case MVM_OP_sp_get_i64:
                case MVM_OP_sp_get_n:
                case MVM_OP_sp_get_s:
                case MVM_OP_sp_p6oget_i:
                case MVM_OP_sp_p6oget_n:
                case MVM_OP_sp_p6oget_s:
                case MVM_OP_sp_p6oget_o: {
                    MVMSpeshFacts *target = MVM_spesh_get_facts(tc, g, ins->operands[1]);
                    MVMSpeshPEAAllocation *alloc = target->pea.allocation;
                    if (allocation_tracked(tc, gs, bb, alloc))
                        add_object_read_transform(tc, g, bb, ins, gs, alloc);
                    break;
                }
                case MVM_OP_sp_get_bi: {
                    MVMSpeshFacts *target = MVM_spesh_get_facts(tc, g, ins->operands[1]);
                    MVMSpeshPEAAllocation *alloc = target->pea.allocation;
                    if (allocation_tracked(tc, gs, bb, alloc)) {
                        Transformation *tran = MVM_spesh_alloc(tc, g, sizeof(Transformation));
                        tran->allocation = alloc;
                        tran->transform = TRANSFORM_GETATTR_TO_SET;
                        tran->attr.ins = ins;
                        tran->attr.hypothetical_reg_idx = find_bigint_register(tc, alloc);
                        add_transform_for_bb(tc, gs, bb, tran);
                        alloc->read = 1;
                    }
                    break;
                }
                case MVM_OP_add_I:
                    if (decompose_and_track_bigint_bi(tc, g, gs, bb, ins, MVM_OP_sp_add_bi))
                        found_replaceable = 1;
                    break;
                case MVM_OP_sub_I:
                    if (decompose_and_track_bigint_bi(tc, g, gs, bb, ins, MVM_OP_sp_sub_bi))
                        found_replaceable = 1;
                    break;
                case MVM_OP_mul_I:
                    if (decompose_and_track_bigint_bi(tc, g, gs, bb, ins, MVM_OP_sp_mul_bi))
                        found_replaceable = 1;
                    break;
                case MVM_OP_gcd_I:
                    if (decompose_and_track_bigint_bi(tc, g, gs, bb, ins, MVM_OP_sp_gcd_bi))
                        found_replaceable = 1;
                    break;
                case MVM_OP_neg_I:
                    if (decompose_and_track_bigint_un(tc, g, gs, bb, ins, MVM_OP_sp_neg_bi))
                        found_replaceable = 1;
                    break;
                case MVM_OP_abs_I:
                    if (decompose_and_track_bigint_un(tc, g, gs, bb, ins, MVM_OP_sp_abs_bi))
                        found_replaceable = 1;
                    break;
                case MVM_OP_cmp_I:
                    if (decompose_bigint_relational(tc, g, gs, bb, ins, MVM_OP_sp_cmp_bi))
                        found_replaceable = 1;
                    break;
                case MVM_OP_eq_I:
                    if (decompose_bigint_relational(tc, g, gs, bb, ins, MVM_OP_sp_eq_bi))
                        found_replaceable = 1;
                    break;
                case MVM_OP_ne_I:
                    if (decompose_bigint_relational(tc, g, gs, bb, ins, MVM_OP_sp_ne_bi))
                        found_replaceable = 1;
                    break;
                case MVM_OP_lt_I:
                    if (decompose_bigint_relational(tc, g, gs, bb, ins, MVM_OP_sp_lt_bi))
                        found_replaceable = 1;
                    break;
                case MVM_OP_le_I:
                    if (decompose_bigint_relational(tc, g, gs, bb, ins, MVM_OP_sp_le_bi))
                        found_replaceable = 1;
                    break;
                case MVM_OP_gt_I:
                    if (decompose_bigint_relational(tc, g, gs, bb, ins, MVM_OP_sp_gt_bi))
                        found_replaceable = 1;
                    break;
                case MVM_OP_ge_I:
                    if (decompose_bigint_relational(tc, g, gs, bb, ins, MVM_OP_sp_ge_bi))
                        found_replaceable = 1;
                    break;
                case MVM_OP_decont_i: {
                    MVMSpeshFacts *target = MVM_spesh_get_facts(tc, g, ins->operands[1]);
                    MVMSpeshPEAAllocation *alloc = target->pea.allocation;
                    if (!(allocation_tracked(tc, gs, bb, alloc) &&
                                try_replace_decont_i(tc, g, gs, bb, ins, alloc)))
                        unhandled_instruction(tc, g, bb, ins, gs);
                    break;
                }
                case MVM_OP_sp_guardconc:
                    if (settified_guard) {
                        /* Guard behaves like an (eliminated) set; track aliasing. */
                        MVMSpeshFacts *source = MVM_spesh_get_facts(tc, g, ins->operands[1]);
                        MVMSpeshPEAAllocation *alloc = source->pea.allocation;
                        if (allocation_tracked(tc, gs, bb, alloc)) {
                            MVM_spesh_get_facts(tc, g, ins->operands[0])->pea.allocation = alloc;
                            add_tracked_register(tc, gs, ins->operands[0], alloc);
                            MVM_spesh_copy_facts_resolved(tc, g,
                                    MVM_spesh_get_facts(tc, g, ins->operands[0]),
                                    source);
                        }
                    }
                    else {
                        /* Guard will really happen; need the real object. */
                        real_object_required(tc, g, bb, ins, ins->operands[1], gs, 1);
                    }
                    break;
                case MVM_OP_prof_allocated: {
                    MVMSpeshFacts *target = MVM_spesh_get_facts(tc, g, ins->operands[0]);
                    MVMSpeshPEAAllocation *alloc = target->pea.allocation;
                    if (allocation_tracked(tc, gs, bb, alloc)) {
                        Transformation *tran = MVM_spesh_alloc(tc, g, sizeof(Transformation));
                        tran->allocation = alloc;
                        tran->transform = TRANSFORM_PROF_ALLOCATED;
                        tran->prof.ins = ins;
                        add_transform_for_bb(tc, gs, bb, tran);
                    }
                }
                case MVM_SSA_PHI: {
                    /* If a PHI doesn't really merge anything, and its input is
                     * a tracked object, we just alias the output. */
                    MVMuint16 num_operands = ins->info->num_operands;
                    if (num_operands == 2) {
                        MVMSpeshFacts *source = MVM_spesh_get_facts(tc, g, ins->operands[1]);
                        MVMSpeshPEAAllocation *alloc = source->pea.allocation;
                        if (allocation_tracked(tc, gs, bb, alloc)) {
                            MVMSpeshFacts *target = MVM_spesh_get_facts(tc, g, ins->operands[0]);
                            target->pea.allocation = alloc;
                            MVM_spesh_copy_facts_resolved(tc, g, target, source);
                        }
                    }
                    else {
                        /* Otherwise, mark the objects involved as irreplaceable
                         * for now (this is a bit awkward, since to do better
                         * we should figure out which branches the PHIs merge
                         * from and place materializations into those.) */
                        MVMuint32 i = 0;
                        for (i = 1; i < ins->info->num_operands; i++)
                            real_object_required(tc, g, bb, ins, ins->operands[i], gs, 0);
                    }
                    break;
                }
                default: {
                    /* Other instructions using tracked objects require the
                     * real object. */
                   unhandled_instruction(tc, g, bb, ins, gs);
                   break;
               }
            }

            ins = ins->next;
            ins_count++;
        }

        seen[bb->rpo_idx] = 1;
    }
    gs->rpo = NULL;
    MVM_free(rpo);
    MVM_free(seen);
    return found_replaceable;
}

void MVM_spesh_pea(MVMThreadContext *tc, MVMSpeshGraph *g) {
    MVMuint32 i;

    GraphState gs;
    memset(&gs, 0, sizeof(GraphState));
    MVM_VECTOR_INIT(gs.tracked_allocations, 0);
    MVM_VECTOR_INIT(gs.shadow_facts, 0);
    MVM_VECTOR_INIT(gs.tracked_registers, 0);
    gs.bb_states = MVM_spesh_alloc(tc, g, g->num_bbs * sizeof(BBState));
    for (i = 0; i < g->num_bbs; i++)
        MVM_VECTOR_INIT(gs.bb_states[i].transformations, 0);

    if (PEA_LOG) {
        char *sf_name = MVM_string_utf8_encode_C_string(tc, g->sf->body.name);
        char *sf_cuuid = MVM_string_utf8_encode_C_string(tc, g->sf->body.cuuid);
        pea_log("considering frame '%s' (%s)", sf_name, sf_cuuid);
        MVM_free(sf_name);
        MVM_free(sf_cuuid);
    }

    if (analyze(tc, g, &gs)) {
        MVMSpeshBB *bb = g->entry;
        gs.attr_regs = MVM_spesh_alloc(tc, g, gs.latest_hypothetical_reg_idx * sizeof(MVMuint16));
        while (bb) {
            for (i = 0; i < MVM_VECTOR_ELEMS(gs.bb_states[bb->idx].transformations); i++)
                apply_transform(tc, g, &gs, bb, gs.bb_states[bb->idx].transformations[i]);
            bb = bb->linear_next;
        }
    }

    for (i = 0; i < g->num_bbs; i++) {
        MVMint32 j;
        for (j = 0; j < MVM_VECTOR_ELEMS(gs.bb_states[i].alloc_state); j++) {
            MVM_VECTOR_DESTROY(gs.bb_states[i].alloc_state[j].materializations);
            MVM_free(gs.bb_states[i].alloc_state[j].used);
        }
        MVM_VECTOR_DESTROY(gs.bb_states[i].alloc_state);
        MVM_VECTOR_DESTROY(gs.bb_states[i].transformations);
    }
    MVM_VECTOR_DESTROY(gs.tracked_allocations);
    MVM_VECTOR_DESTROY(gs.shadow_facts);
    for (i = 0; i < MVM_VECTOR_ELEMS(gs.tracked_registers); i++)
        MVM_VECTOR_DESTROY(gs.tracked_registers[i].allocation->escape_dependencies);
    MVM_VECTOR_DESTROY(gs.tracked_registers);
}

/* Clean up any deopt info. */
void MVM_spesh_pea_destroy_deopt_info(MVMThreadContext *tc, MVMSpeshPEADeopt *deopt_pea) {
    MVMint32 i;
    for (i = 0; i < MVM_VECTOR_ELEMS(deopt_pea->materialize_info); i++)
        MVM_free(deopt_pea->materialize_info[i].attr_regs);
    MVM_VECTOR_DESTROY(deopt_pea->materialize_info);
    MVM_VECTOR_DESTROY(deopt_pea->deopt_point);
}
