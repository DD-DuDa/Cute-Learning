/******************************************************************************
 * Copyright (c) 2024, Tri Dao.
 ******************************************************************************/

#pragma once

#include <cute/tensor.hpp>

#include <cutlass/cutlass.h>
#include <cutlass/array.h>
#include <cutlass/numeric_types.h>

#include "include/block_info.h"
#include "include/kernel_traits.h"
#include "include/utils.h"
#include "include/softmax.h"
#include "include/mask.h"
#include "include/dropout.h"
#include "include/rotary.h"


namespace flash {

using namespace cute;

template <typename Engine, typename Layout>
__forceinline__ __device__ void apply_softcap(Tensor<Engine, Layout> &tensor, const float softcap){
    #pragma unroll
    for (int i = 0; i < size(tensor); ++i) {
        tensor(i) = cutlass::fast_tanh(tensor(i) * softcap);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

template<typename ElementAccum, typename Params, int kBlockM, bool Is_even_MN>
__forceinline__ __device__ auto get_lse_tile(const Params &params, const int bidb, const int bidh, const int m_block, const BlockInfo</*Varlen=*/!Is_even_MN> &binfo) {
        // When params.unpadded_lse is false, LSE is written as (b, h, seqlen_q) - this is non-variable seqlen path.
        // Otherwise, when params.seqlenq_ngroups_swapped is true, it is written as (h, seqlen_q, b) to account for seqlen_q <-> h swapping trick.
        // Otherwise, it's written as (h, b, seqlen_q).
        const bool varlen_q = params.unpadded_lse && !params.seqlenq_ngroups_swapped;
        auto lse_offset = varlen_q ? binfo.q_offset(params.seqlen_q, 1, bidb) : 0;
        auto gmem_ptr_lse = make_gmem_ptr(reinterpret_cast<ElementAccum*>(params.softmax_lse_ptr) + lse_offset);

        auto lse_shape = varlen_q ? make_shape(1, params.h, params.total_q) : make_shape(params.b, params.h, params.seqlen_q);
        auto lse_stride = params.seqlenq_ngroups_swapped ? make_stride(1, params.seqlen_q * params.b, params.b) : (
            params.unpadded_lse ? make_stride(params.h * params.total_q, params.total_q, 1) :  make_stride(params.h * params.seqlen_q, params.seqlen_q, 1)
            );

        auto lse_layout = make_layout(lse_shape, lse_stride);
        Tensor mLSE = make_tensor(gmem_ptr_lse, lse_layout);
        auto mLSE_slice = varlen_q ? mLSE(0, bidh, _) : mLSE(bidb, bidh, _);
        return local_tile(mLSE_slice, Shape<Int<kBlockM>>{}, make_coord(m_block));
}

////////////////////////////////////////////////////////////////////////////////////////////////////

template<typename Kernel_traits, bool Is_causal, bool Is_local, bool Has_alibi, bool Is_even_MN, bool Is_even_K, bool Is_softcap, bool Split, bool Append_KV, typename Params>
inline __device__ void compute_attn_1rowblock_splitkv(const Params &params, const int bidb, const int bidh, const int m_block, const int n_split_idx, const int num_n_splits) {

    using Element = typename Kernel_traits::Element;
    using ElementAccum = typename Kernel_traits::ElementAccum;
    using index_t = typename Kernel_traits::index_t;

    // Shared memory.
    extern __shared__ char smem_[];

    // The thread index.
    const int tidx = threadIdx.x;

    constexpr int kBlockM = Kernel_traits::kBlockM;
    constexpr int kBlockN = Kernel_traits::kBlockN;
    constexpr int kHeadDim = Kernel_traits::kHeadDim;
    constexpr int kNWarps = Kernel_traits::kNWarps;

    using GmemTiledCopyO = std::conditional_t<
        !Split,
        typename Kernel_traits::GmemTiledCopyO,
        typename Kernel_traits::GmemTiledCopyOaccum
    >;
    using ElementO = std::conditional_t<!Split, Element, ElementAccum>;

    const BlockInfo</*Varlen=*/!Is_even_MN> binfo(params, bidb);
    if (m_block * kBlockM >= binfo.actual_seqlen_q) return;

    const int n_blocks_per_split = ((binfo.actual_seqlen_k + kBlockN - 1) / kBlockN + num_n_splits - 1) / num_n_splits;
    const int n_block_min = n_split_idx * n_blocks_per_split;
    int n_block_max = std::min(cute::ceil_div(binfo.actual_seqlen_k, kBlockN), (n_split_idx + 1) * n_blocks_per_split);

    if (n_block_min >= n_block_max) {  // This also covers the case where n_block_max <= 0
        // We exit early and write 0 to gOaccum and -inf to gLSEaccum.
        // Otherwise we might read OOB elements from gK and gV,
        // or get wrong results when we combine gOaccum from different blocks.
        const index_t row_offset_o = binfo.q_offset(params.o_batch_stride, params.o_row_stride, bidb)
            + m_block * kBlockM * params.o_row_stride + bidh * params.o_head_stride;
        const index_t row_offset_oaccum = (((n_split_idx * params.b + bidb) * params.h + bidh) * params.seqlen_q
            + m_block * kBlockM) * params.d_rounded;
        const index_t row_offset_lseaccum = ((n_split_idx * params.b + bidb) * params.h + bidh) * params.seqlen_q + m_block * kBlockM;
        Tensor gOaccum = make_tensor(make_gmem_ptr(reinterpret_cast<ElementO *>(Split ? params.oaccum_ptr : params.o_ptr) + (Split ? row_offset_oaccum : row_offset_o)),
                                      Shape<Int<kBlockM>, Int<kHeadDim>>{},
                                     make_stride(Split ? kHeadDim : params.o_row_stride, _1{}));
        Tensor gLSEaccum = make_tensor(make_gmem_ptr(reinterpret_cast<ElementAccum *>(Split ? params.softmax_lseaccum_ptr : params.softmax_lse_ptr) + row_offset_lseaccum),
                                      Shape<Int<kBlockM>>{}, Stride<_1>{});

        GmemTiledCopyO gmem_tiled_copy_Oaccum;
        auto gmem_thr_copy_Oaccum = gmem_tiled_copy_Oaccum.get_thread_slice(tidx);
        Tensor tOgOaccum = gmem_thr_copy_Oaccum.partition_D(gOaccum);
        Tensor tOrOaccum = make_tensor<ElementO>(shape(tOgOaccum));
        clear(tOrOaccum);
        // Construct identity layout for sO
        Tensor cO = make_identity_tensor(make_shape(size<0>(gOaccum), size<1>(gOaccum)));    // (BLK_M,BLK_K) -> (blk_m,blk_k)
        // Repeat the partitioning with identity layouts
        Tensor tOcO = gmem_thr_copy_Oaccum.partition_D(cO);
        Tensor tOpO = make_tensor<bool>(make_shape(size<2>(tOgOaccum)));
        if (!Is_even_K) {
            #pragma unroll
            for (int k = 0; k < size(tOpO); ++k) { tOpO(k) = get<1>(tOcO(0, 0, k)) < params.d; }
        }
        // Clear_OOB_K must be false since we don't want to write zeros to gmem
        flash::copy<Is_even_MN, Is_even_K, /*Clear_OOB_MN=*/false, /*Clear_OOB_K=*/false>(
            gmem_tiled_copy_Oaccum, tOrOaccum, tOgOaccum, tOcO, tOpO, binfo.actual_seqlen_q - m_block * kBlockM
        );
        #pragma unroll
        for (int m = 0; m < size<1>(tOgOaccum); ++m) {
            const int row = get<0>(tOcO(0, m, 0));
            if (row < binfo.actual_seqlen_q - m_block * kBlockM && get<1>(tOcO(0, m, 0)) == 0) { gLSEaccum(row) = Split ? -INFINITY : INFINITY; }
        }
        return;
    }
    // We iterate over the blocks in reverse order. This is because the last block is the only one
    // that needs masking when we read K and V from global memory. Moreover, iterating in reverse
    // might save us 1 register (we just need n_block instead of both n_block and n_block_max).

    // We move K and V to the last block.
    const int bidb_cache = bidb;
    const int *block_table = nullptr;
    const int block_table_idx = 0;
    const int block_table_offset = 0;
    const index_t row_offset_k = binfo.k_offset(params.k_batch_stride, params.k_row_stride, bidb_cache)
          + (n_block_max - 1) * kBlockN * params.k_row_stride + (bidh / params.h_h_k_ratio) * params.k_head_stride;
    const index_t row_offset_v = binfo.k_offset(params.v_batch_stride, params.v_row_stride, bidb_cache)
          + (n_block_max - 1) * kBlockN * params.v_row_stride + (bidh / params.h_h_k_ratio) * params.v_head_stride;

    // Global memory tensors
    Tensor mQ = make_tensor(make_gmem_ptr(reinterpret_cast<Element*>(params.q_ptr) + binfo.q_offset(params.q_batch_stride, params.q_row_stride, bidb)),
                            make_shape(binfo.actual_seqlen_q, params.h, params.d),
                            make_stride(params.q_row_stride, params.q_head_stride, _1{}));
    Tensor gQ = local_tile(mQ(_, bidh, _), Shape<Int<kBlockM>, Int<kHeadDim>>{},
                           make_coord(m_block, 0));  // (kBlockM, kHeadDim)
    Tensor gK = make_tensor(make_gmem_ptr(reinterpret_cast<Element *>(params.k_ptr) + row_offset_k),
                            Shape<Int<kBlockN>, Int<kHeadDim>>{},
                            make_stride(params.k_row_stride, _1{}));
    Tensor gV = make_tensor(make_gmem_ptr(reinterpret_cast<Element *>(params.v_ptr) + row_offset_v),
                            Shape<Int<kBlockN>, Int<kHeadDim>>{},
                            make_stride(params.v_row_stride, _1{}));

    // Shared memory tensors
    Tensor sQ = make_tensor(make_smem_ptr(reinterpret_cast<Element *>(smem_)),
                            typename Kernel_traits::SmemLayoutQ{});
    Tensor sK = make_tensor(sQ.data() + size(sQ), typename Kernel_traits::SmemLayoutKV{});
    Tensor sV = make_tensor(sK.data() + size(sK), typename Kernel_traits::SmemLayoutKV{});
    Tensor sVt = make_tensor(sV.data(), typename Kernel_traits::SmemLayoutVtransposed{});
    Tensor sVtNoSwizzle = make_tensor(sV.data().get(), typename Kernel_traits::SmemLayoutVtransposedNoSwizzle{});

    // Copy, global memory to shared memory
    typename Kernel_traits::GmemTiledCopyQKV gmem_tiled_copy_QKV;

    auto gmem_thr_copy_QKV = gmem_tiled_copy_QKV.get_thread_slice(tidx);
    Tensor tQgQ = gmem_thr_copy_QKV.partition_S(gQ);
    Tensor tQsQ = gmem_thr_copy_QKV.partition_D(sQ);
    Tensor tKgK = gmem_thr_copy_QKV.partition_S(gK);  // (KCPY, KCPY_N, KCPY_K)
    Tensor tKsK = gmem_thr_copy_QKV.partition_D(sK);
    Tensor tVgV = gmem_thr_copy_QKV.partition_S(gV);  // (VCPY, VCPY_N, VCPY_K)
    Tensor tVsV = gmem_thr_copy_QKV.partition_D(sV);

    // Register memory tensors
    typename Kernel_traits::TiledMma tiled_mma;
    auto thr_mma = tiled_mma.get_thread_slice(tidx);
    Tensor tSrQ  = thr_mma.partition_fragment_A(sQ);                           // (MMA,MMA_M,MMA_K)
    Tensor tSrK  = thr_mma.partition_fragment_B(sK);                           // (MMA,MMA_N,MMA_K)
    Tensor tOrVt  = thr_mma.partition_fragment_B(sVtNoSwizzle);                // (MMA, MMA_K,MMA_N)

    Tensor acc_o = partition_fragment_C(tiled_mma, Shape<Int<kBlockM>, Int<kHeadDim>>{});  // MMA, MMA_M, MMA_K

    // Copy, shared memory to register memory
    auto smem_tiled_copy_Q = make_tiled_copy_A(typename Kernel_traits::SmemCopyAtom{}, tiled_mma);
    auto smem_thr_copy_Q = smem_tiled_copy_Q.get_thread_slice(tidx);
    Tensor tSsQ = smem_thr_copy_Q.partition_S(sQ);

    auto smem_tiled_copy_K = make_tiled_copy_B(typename Kernel_traits::SmemCopyAtom{}, tiled_mma);
    auto smem_thr_copy_K = smem_tiled_copy_K.get_thread_slice(tidx);
    Tensor tSsK = smem_thr_copy_K.partition_S(sK);

    auto smem_tiled_copy_V = make_tiled_copy_B(typename Kernel_traits::SmemCopyAtomTransposed{}, tiled_mma);
    auto smem_thr_copy_V = smem_tiled_copy_V.get_thread_slice(tidx);
    Tensor tOsVt = smem_thr_copy_V.partition_S(sVt);

    // PREDICATES

    // Construct identity layout for sQ and sK
    Tensor cQ = make_identity_tensor(make_shape(size<0>(sQ), size<1>(sQ)));    // (BLK_M,BLK_K) -> (blk_m,blk_k)
    Tensor cKV = make_identity_tensor(make_shape(size<0>(sK), size<1>(sK)));    // (BLK_N,BLK_K) -> (blk_n,blk_k)

    // Repeat the partitioning with identity layouts
    Tensor tQcQ = gmem_thr_copy_QKV.partition_S(cQ);       // (ACPY,ACPY_M,ACPY_K) -> (blk_m,blk_k)
    Tensor tKcK = gmem_thr_copy_QKV.partition_S(cKV);       // (BCPY,BCPY_N,BCPY_K) -> (blk_n,blk_k)
    Tensor tKVcKV = gmem_thr_copy_QKV.partition_S(cKV);   // (BCPY,BCPY_N,BCPY_K) -> (blk_n,blk_k)

    // Allocate predicate tensors for k
    Tensor tQpQ = make_tensor<bool>(make_shape(size<2>(tQsQ)));
    Tensor tKVpKV = make_tensor<bool>(make_shape(size<2>(tKsK)));

    // Set predicates for k bounds
    if (!Is_even_K) {
        #pragma unroll
        for (int k = 0; k < size(tQpQ); ++k) { tQpQ(k) = get<1>(tQcQ(0, 0, k)) < params.d; }
        #pragma unroll
        for (int k = 0; k < size(tKVpKV); ++k) { tKVpKV(k) = get<1>(tKVcKV(0, 0, k)) < params.d; }
    }

    // Prologue

    // Read Q from gmem to smem, optionally apply rotary embedding.
    if (!Append_KV || params.rotary_dim == 0) {
        // We don't need to clear the sQ smem tiles since we'll only write out the valid outputs
        flash::copy<Is_even_MN, Is_even_K>(gmem_tiled_copy_QKV, tQgQ, tQsQ, tQcQ, tQpQ,
                                           binfo.actual_seqlen_q - m_block * kBlockM);
    }

    int n_block = n_block_max - 1;
    // We don't need to clear the sK smem tiles since we'll mask out the scores anyway.
    flash::copy<Is_even_MN, Is_even_K>(gmem_tiled_copy_QKV, tKgK, tKsK, tKcK, tKVpKV,
                                       binfo.actual_seqlen_k - n_block * kBlockN);
    cute::cp_async_fence();


    clear(acc_o);

    flash::Softmax<2 * size<1>(acc_o)> softmax;
    const float alibi_slope = !Has_alibi ? 0.0f : reinterpret_cast<float *>(params.alibi_slopes_ptr)[bidb * params.alibi_slopes_batch_stride + bidh] / params.scale_softmax;
    flash::Mask<Is_causal, Is_local, Has_alibi> mask(binfo.actual_seqlen_k, binfo.actual_seqlen_q, params.window_size_left, params.window_size_right, alibi_slope);

    if (threadIdx.x == 0 && blockIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0) {
        
    }

    constexpr int n_masking_steps = 1;
    #pragma unroll
    for (int masking_step = 0; masking_step < n_masking_steps; ++masking_step, --n_block) {
        Tensor acc_s = partition_fragment_C(tiled_mma, Shape<Int<kBlockM>, Int<kBlockN>>{});  // (MMA=4, MMA_M, MMA_N)
        clear(acc_s);
        flash::cp_async_wait<0>();
        __syncthreads();

        // Advance gV
        if (masking_step > 0) {
            if (block_table == nullptr) {
                tVgV.data() = tVgV.data() + (-int(kBlockN * params.v_row_stride));
            } 
            flash::copy</*Is_even_MN=*/true, Is_even_K>(gmem_tiled_copy_QKV, tVgV, tVsV, tKVcKV, tKVpKV);
        } else {
            // Clear the smem tiles to account for predicated off loads
            flash::copy<Is_even_MN, Is_even_K, /*Clear_OOB_MN=*/true>(
                gmem_tiled_copy_QKV, tVgV, tVsV, tKVcKV, tKVpKV, binfo.actual_seqlen_k - n_block * kBlockN
            );
        }
        cute::cp_async_fence();

        flash::gemm(
            acc_s, tSrQ, tSrK, tSsQ, tSsK, tiled_mma, smem_tiled_copy_Q, smem_tiled_copy_K,
            smem_thr_copy_Q, smem_thr_copy_K
        );


        mask.template apply_mask<Is_causal, Is_even_MN>(
            acc_s, n_block * kBlockN, m_block * kBlockM + (tidx / 32) * 16 + (tidx % 32) / 4, kNWarps * 16
        );

        flash::cp_async_wait<0>();
        __syncthreads();

        if (n_block > n_block_min) {
            // Advance gK
            tKgK.data() = tKgK.data() + (-int(kBlockN * params.k_row_stride));
            flash::copy</*Is_even_MN=*/true, Is_even_K>(gmem_tiled_copy_QKV, tKgK, tKsK, tKcK, tKVpKV);
            // This cp_async_fence needs to be in the if block, otherwise the synchronization
            // isn't right and we get race conditions.
            cute::cp_async_fence();
        }

        // We have key_padding_mask so we'll need to Check_inf
        masking_step == 0
            ? softmax.template softmax_rescale_o</*Is_first=*/true,  /*Check_inf=*/Is_causal || Is_local || !Is_even_MN>(acc_s, acc_o, params.scale_softmax_log2)
            : softmax.template softmax_rescale_o</*Is_first=*/false, /*Check_inf=*/Is_causal || Is_local || !Is_even_MN>(acc_s, acc_o, params.scale_softmax_log2);

        // Convert acc_s from fp32 to fp16/bf16
        Tensor rP = flash::convert_type<Element>(acc_s);
        // Reshape rP from (MMA=4, MMA_M, MMA_N) to ((4, 2), MMA_M, MMA_N / 2)
        // if using m16n8k16 or (4, MMA_M, MMA_N) if using m16n8k8.
        Tensor tOrP = make_tensor(rP.data(), flash::convert_layout_acc_Aregs<Kernel_traits::TiledMma>(rP.layout()));

        flash::gemm_rs(acc_o, tOrP, tOrVt, tOsVt, tiled_mma, smem_tiled_copy_V, smem_thr_copy_V);

        // This check is at the end of the loop since we always have at least 1 iteration
        if (n_masking_steps > 1 && n_block <= n_block_min) {
            --n_block;
            break;
        }

    }

    // These are the iterations where we don't need masking on S
    for (; n_block >= n_block_min; --n_block) {
        Tensor acc_s = partition_fragment_C(tiled_mma, Shape<Int<kBlockM>, Int<kBlockN>>{});  // (MMA=4, MMA_M, MMA_N)
        clear(acc_s);
        flash::cp_async_wait<0>();
        __syncthreads();
        // Advance gV
        tVgV.data() = tVgV.data() + (-int(kBlockN * params.v_row_stride));

        flash::copy</*Is_even_MN=*/true, Is_even_K>(gmem_tiled_copy_QKV, tVgV, tVsV, tKVcKV, tKVpKV);
        cute::cp_async_fence();

        flash::gemm(
            acc_s, tSrQ, tSrK, tSsQ, tSsK, tiled_mma, smem_tiled_copy_Q, smem_tiled_copy_K,
            smem_thr_copy_Q, smem_thr_copy_K
        );
        if constexpr (Is_softcap){
            apply_softcap(acc_s, params.softcap);
        }

        flash::cp_async_wait<0>();
        __syncthreads();

        if (n_block > n_block_min) {
            // Advance gK
            if (block_table == nullptr) {
                tKgK.data() = tKgK.data() + (-int(kBlockN * params.k_row_stride));
            } else {
                const int block_table_idx_cur = n_block * kBlockN / params.page_block_size;
                const int block_table_offset_cur = n_block * kBlockN - block_table_idx_cur * params.page_block_size;
                const int block_table_idx_next = (n_block - 1) * kBlockN / params.page_block_size;
                const int block_table_offset_next = (n_block - 1) * kBlockN - block_table_idx_next * params.page_block_size;
                tKgK.data() = tKgK.data() + (block_table[block_table_idx_next] - block_table[block_table_idx_cur]) * params.k_batch_stride + (block_table_offset_next - block_table_offset_cur) * params.k_row_stride;
            }
            flash::copy</*Is_even_MN=*/true, Is_even_K>(gmem_tiled_copy_QKV, tKgK, tKsK, tKcK, tKVpKV);
            // This cp_async_fence needs to be in the if block, otherwise the synchronization
            // isn't right and we get race conditions.
            cute::cp_async_fence();
        }

        // mask.template apply_mask</*Causal_mask=*/false>(
        //     acc_s, n_block * kBlockN, m_block * kBlockM + (tidx / 32) * 16 + (tidx % 32) / 4, kNWarps * 16
        // );
        softmax.template softmax_rescale_o</*Is_first=*/false, /*Check_inf=*/Is_local>(acc_s, acc_o, params.scale_softmax_log2);

        Tensor rP = flash::convert_type<Element>(acc_s);
        // Reshape rP from (MMA=4, MMA_M, MMA_N) to ((4, 2), MMA_M, MMA_N / 2)
        // if using m16n8k16 or (4, MMA_M, MMA_N) if using m16n8k8.
        Tensor tOrP = make_tensor(rP.data(), flash::convert_layout_acc_Aregs<Kernel_traits::TiledMma>(rP.layout()));

        flash::gemm_rs(acc_o, tOrP, tOrVt, tOsVt, tiled_mma, smem_tiled_copy_V, smem_thr_copy_V);

    }
    // Epilogue

    Tensor lse = softmax.template normalize_softmax_lse</*Is_dropout=*/false, Split>(acc_o, params.scale_softmax);
    // if (cute::thread0()) { print(lse); }

    Tensor sOaccum = make_tensor(make_smem_ptr(reinterpret_cast<ElementO *>(smem_)), typename Kernel_traits::SmemLayoutO{}); // (SMEM_M,SMEM_N)
    // Partition sO to match the accumulator partitioning
    using SmemTiledCopyO = std::conditional_t<
        !Split,
        typename Kernel_traits::SmemCopyAtomO,
        typename Kernel_traits::SmemCopyAtomOaccum
    >;
    auto smem_tiled_copy_Oaccum = make_tiled_copy_C(SmemTiledCopyO{}, tiled_mma);
    auto smem_thr_copy_Oaccum = smem_tiled_copy_Oaccum.get_thread_slice(tidx);
    Tensor rO = flash::convert_type<ElementO>(acc_o);
    Tensor taccOrOaccum = smem_thr_copy_Oaccum.retile_S(rO);        // ((Atom,AtomNum), MMA_M, MMA_N)
    Tensor taccOsOaccum = smem_thr_copy_Oaccum.partition_D(sOaccum);     // ((Atom,AtomNum),PIPE_M,PIPE_N)

    // sOaccum is larger than sQ, so we need to syncthreads here
    // TODO: allocate enough smem for sOaccum
    if constexpr (Split) { __syncthreads(); }

    cute::copy(smem_tiled_copy_Oaccum, taccOrOaccum, taccOsOaccum);

    const index_t row_offset_o = binfo.q_offset(params.o_batch_stride, params.o_row_stride, bidb)
        + m_block * kBlockM * params.o_row_stride + bidh * params.o_head_stride;
    const index_t row_offset_oaccum = (((n_split_idx * params.b + bidb) * params.h + bidh) * params.seqlen_q
                                         + m_block * kBlockM) * params.d_rounded;
    const index_t row_offset_lseaccum = (Split || !params.unpadded_lse ?
            ((n_split_idx * params.b + bidb) * params.h + bidh) * params.seqlen_q : bidh * params.total_q + binfo.q_offset(params.seqlen_q, 1, bidb)
        ) + m_block * kBlockM;

    Tensor gOaccum = make_tensor(make_gmem_ptr(reinterpret_cast<ElementO *>(Split ? params.oaccum_ptr : params.o_ptr) + (Split ? row_offset_oaccum : row_offset_o)),
                                 Shape<Int<kBlockM>, Int<kHeadDim>>{},
                                 make_stride(Split ? kHeadDim : params.o_row_stride, _1{}));
    Tensor gLSEaccum = make_tensor(make_gmem_ptr(reinterpret_cast<ElementAccum *>(Split ? params.softmax_lseaccum_ptr : params.softmax_lse_ptr) + row_offset_lseaccum),
                                   Shape<Int<kBlockM>>{}, Stride<_1>{});
    // if (tidx == 0) { printf("row_offset_o = %d, bidh = %d, gOaccum = %p\n", row_offset_o, bidh, gOaccum.data()); }

    GmemTiledCopyO gmem_tiled_copy_Oaccum;
    auto gmem_thr_copy_Oaccum = gmem_tiled_copy_Oaccum.get_thread_slice(tidx);
    Tensor tOsOaccum = gmem_thr_copy_Oaccum.partition_S(sOaccum);        // ((Atom,AtomNum),ATOM_M,ATOM_N)
    Tensor tOgOaccum = gmem_thr_copy_Oaccum.partition_D(gOaccum);

    __syncthreads();

    Tensor tOrOaccum = make_tensor<ElementO>(shape(tOgOaccum));
    cute::copy(gmem_tiled_copy_Oaccum, tOsOaccum, tOrOaccum);

    Tensor caccO = make_identity_tensor(Shape<Int<kBlockM>, Int<kHeadDim>>{});    // (BLK_M,BLK_K) -> (blk_m,blk_k)
    Tensor taccOcO = thr_mma.partition_C(caccO);                           // (MMA,MMA_M,MMA_K)
    static_assert(decltype(size<0>(taccOcO))::value == 4);
    // Convert to ((2, 2), MMA_M, MMA_K) then take only the row indices.
    Tensor taccOcO_row = logical_divide(taccOcO, Shape<_2>{})(make_coord(0, _), _, 0);
    CUTE_STATIC_ASSERT_V(size(lse) == size(taccOcO_row));                     // MMA_M
    if (get<1>(taccOcO_row(0)) == 0) {
        #pragma unroll
        for (int mi = 0; mi < size(lse); ++mi) {
            const int row = get<0>(taccOcO_row(mi));
            if (row < binfo.actual_seqlen_q - m_block * kBlockM) { gLSEaccum(row) = lse(mi); }
        }
    }

    // Construct identity layout for sO
    Tensor cO = make_identity_tensor(make_shape(size<0>(sOaccum), size<1>(sOaccum)));    // (BLK_M,BLK_K) -> (blk_m,blk_k)
    // Repeat the partitioning with identity layouts
    Tensor tOcO = gmem_thr_copy_Oaccum.partition_D(cO);                           // (ACPY,ACPY_M,ACPY_K) -> (blk_m,blk_k)
    Tensor tOpO = make_tensor<bool>(make_shape(size<2>(tOgOaccum)));
    if (!Is_even_K) {
        #pragma unroll
        for (int k = 0; k < size(tOpO); ++k) { tOpO(k) = get<1>(tOcO(0, 0, k)) < params.d; }
    }
    // Clear_OOB_K must be false since we don't want to write zeros to gmem
    flash::copy<Is_even_MN, Is_even_K, /*Clear_OOB_MN=*/false, /*Clear_OOB_K=*/false>(
        gmem_tiled_copy_Oaccum, tOrOaccum, tOgOaccum, tOcO, tOpO, binfo.actual_seqlen_q - m_block * kBlockM
    );

    // if (threadIdx.x == 0 && blockIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0) {
    //     printf("gLSEaccum: \n");
    //     print_tensor(gLSEaccum);
    // }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

template<typename Kernel_traits, bool Is_dropout, bool Is_causal, bool Is_local, bool Has_alibi, bool Is_even_MN, bool Is_even_K, bool Is_softcap, bool Return_softmax, typename Params>
inline __device__ void compute_attn(const Params &params) {
    const int m_block = blockIdx.x;
    // The block index for the batch.
    const int bidb = blockIdx.y;
    // The block index for the head.
    const int bidh = blockIdx.z;

    // We want the fwd and bwd to generate the same dropout pattern (RNG), without restricting
    // them to have the same number of threads or have to traverse the attention matrix
    // in the same order.
    // In the Philox RNG, we use the offset to store the batch, head, and the lane id
    // (within a warp). We use the subsequence to store the location of the 16 x 32 blocks within
    // the attention matrix. This way, as long as we have the batch, head, and the location of
    // the 16 x 32 block within the attention matrix, we can generate the exact same dropout pattern.

    // flash::compute_attn_1rowblock<Kernel_traits, Is_dropout, Is_causal, Is_local, Has_alibi, Is_even_MN, Is_even_K, Is_softcap, Return_softmax>(params, bidb, bidh, m_block);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

template<typename Kernel_traits, bool Is_causal, bool Is_local, bool Has_alibi, bool Is_even_MN, bool Is_even_K, bool Is_softcap, bool Split, bool Append_KV, typename Params>
inline __device__ void compute_attn_splitkv(const Params &params) {
    const int m_block = blockIdx.x;
    // The block index for the batch.
    const int bidb = Split ? blockIdx.z / params.h : blockIdx.y;
    // The block index for the head.
    const int bidh = Split ? blockIdx.z - bidb * params.h : blockIdx.z;
    const int n_split_idx = Split ? blockIdx.y : 0;
    const int num_n_splits = Split ? gridDim.y : 1;
    flash::compute_attn_1rowblock_splitkv<Kernel_traits, Is_causal, Is_local, Has_alibi, Is_even_MN, Is_even_K, Is_softcap, Split, Append_KV>(params, bidb, bidh, m_block, n_split_idx, num_n_splits);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

template<typename Kernel_traits, int kBlockM, int Log_max_splits, bool Is_even_K, typename Params>
inline __device__ void combine_attn_seqk_parallel(const Params &params) {
    using Element = typename Kernel_traits::Element;
    using ElementAccum = typename Kernel_traits::ElementAccum;
    using index_t = typename Kernel_traits::index_t;
    constexpr int kMaxSplits = 1 << Log_max_splits;
    constexpr int kHeadDim = Kernel_traits::kHeadDim;
    constexpr int kNThreads = Kernel_traits::kNThreads;

    static_assert(kMaxSplits <= 128, "kMaxSplits must be <= 128");
    static_assert(kBlockM == 4 || kBlockM == 8 || kBlockM == 16 || kBlockM == 32, "kBlockM must be 4, 8, 16 or 32");
    static_assert(kNThreads == 128, "We assume that each block has 128 threads");

    // Shared memory.
    // kBlockM + 1 instead of kBlockM to reduce bank conflicts.
    __shared__ ElementAccum sLSE[kMaxSplits][kBlockM + 1];

    // The thread and block index.
    const int tidx = threadIdx.x;
    const int bidx = blockIdx.x;

    const index_t lse_size = params.b * params.h * params.seqlen_q;

    const index_t row_offset_lse = bidx * kBlockM;
    Tensor gLSEaccum = make_tensor(make_gmem_ptr(reinterpret_cast<ElementAccum *>(params.softmax_lseaccum_ptr) + row_offset_lse),
                                   Shape<Int<kMaxSplits>, Int<kBlockM>>{},
                                   make_stride(lse_size, _1{}));

    // LSE format is different depending on params.unpadded_lse and params.seqlenq_ngroups_swapped, see comment in get_lse_tile.
    // This tensor's layout maps row_offset_lse to {bidb, bidh, q_offset}.
    Tensor gLSE = make_tensor(make_gmem_ptr(reinterpret_cast<ElementAccum *>(params.softmax_lse_ptr) + row_offset_lse),
                              Shape<Int<kBlockM>>{}, Stride<_1>{});

    // This layout maps row_offset_lse to {bidh, q_offset, bidb} or {bidh, bidb, q_offset}.
    Layout flat_layout = make_layout(lse_size);
    Layout orig_layout = make_layout(make_shape(params.seqlen_q, params.h, params.b));
    auto transposed_stride = params.seqlenq_ngroups_swapped ? make_stride(params.b, params.seqlen_q * params.b, 1) : make_stride(1, params.seqlen_q * params.b, params.seqlen_q);
    Layout remapped_layout = make_layout(make_shape(params.seqlen_q, params.h, params.b), transposed_stride);
    Layout final_layout = cute::composition(remapped_layout, cute::composition(orig_layout, flat_layout));

    Tensor gLSE_unpadded = make_tensor(make_gmem_ptr(reinterpret_cast<ElementAccum *>(params.softmax_lse_ptr)), final_layout);

    constexpr int kNLsePerThread = (kMaxSplits * kBlockM + kNThreads - 1) / kNThreads; // 4 * 4 / 128 = 1

    // Read the LSE values from gmem and store them in shared memory, then transpose them.
    constexpr int kRowsPerLoadLSE = kNThreads / kBlockM;
    #pragma unroll
    for (int l = 0; l < kNLsePerThread; ++l) {
        const int row = l * kRowsPerLoadLSE + tidx / kBlockM;
        const int col = tidx % kBlockM;
        ElementAccum lse = (row < params.num_splits && col < lse_size - bidx * kBlockM) ? gLSEaccum(row, col) : -INFINITY;
        if (row < kMaxSplits) { sLSE[row][col] = lse; }
        // if (bidx == 0 && tidx < 32) { printf("tidx = %d, row = %d, col = %d, lse = %f\n", tidx, row, col, lse); }
    }
    // if (bidx == 1 && tidx < 32) { printf("tidx = %d, row_offset_lse = %d, lse = %f\n", tidx, row_offset_lse, lse_accum(0)); }
    __syncthreads();
    Tensor lse_accum = make_tensor<ElementAccum>(Shape<Int<kNLsePerThread>>{});
    constexpr int kRowsPerLoadTranspose = std::min(kRowsPerLoadLSE, kMaxSplits);
    // To make sure that kMaxSplits is within 1 warp: we decide how many elements within kMaxSplits
    // each thread should hold. If kMaxSplits = 16, then each thread holds 2 elements (128 threads,
    // kBlockM rows, so each time we load we can load 128 / kBlockM rows).
    // constexpr int kThreadsPerSplit = kMaxSplits / kRowsPerLoadTranspose;
    // static_assert(kThreadsPerSplit <= 32);
    static_assert(kRowsPerLoadTranspose <= 32);
    static_assert(kNLsePerThread * kRowsPerLoadTranspose <= kMaxSplits);
    #pragma unroll
    for (int l = 0; l < kNLsePerThread; ++l) {
        const int row = l * kRowsPerLoadTranspose + tidx % kRowsPerLoadTranspose;
        const int col = tidx / kRowsPerLoadTranspose;
        lse_accum(l) = (row < kMaxSplits && col < kBlockM) ? sLSE[row][col] : -INFINITY;
        // if (bidx == 0 && tidx < 32) { printf("tidx = %d, row = %d, col = %d, lse = %f\n", tidx, row, col, lse_accum(l)); }
    }

    // Compute the logsumexp of the LSE along the split dimension.
    ElementAccum lse_max = lse_accum(0);
    #pragma unroll
    for (int l = 1; l < kNLsePerThread; ++l) { lse_max = max(lse_max, lse_accum(l)); }
    MaxOp<float> max_op;
    lse_max = Allreduce<kRowsPerLoadTranspose>::run(lse_max, max_op);
    lse_max = lse_max == -INFINITY ? 0.0f : lse_max;  // In case all local LSEs are -inf
    float lse_sum = expf(lse_accum(0) - lse_max);
    #pragma unroll
    for (int l = 1; l < kNLsePerThread; ++l) { lse_sum += expf(lse_accum(l) - lse_max); }
    SumOp<float> sum_op;
    lse_sum = Allreduce<kRowsPerLoadTranspose>::run(lse_sum, sum_op);
    // For the case where all local lse == -INFINITY, we want to set lse_logsum to INFINITY. Otherwise
    // lse_logsum is log(0.0) = -INFINITY and we get NaN when we do lse_accum(l) - lse_logsum.
    ElementAccum lse_logsum = (lse_sum == 0.f || lse_sum != lse_sum) ? INFINITY : logf(lse_sum) + lse_max;
    // if (bidx == 0 && tidx < 32) { printf("tidx = %d, lse = %f, lse_max = %f, lse_logsum = %f\n", tidx, lse_accum(0), lse_max, lse_logsum); }
    if (tidx % kRowsPerLoadTranspose == 0 && tidx / kRowsPerLoadTranspose < kBlockM) {
        if (params.unpadded_lse) {
            const index_t lse_offset = row_offset_lse + tidx / kRowsPerLoadTranspose;
            if (lse_offset < lse_size) {
                gLSE_unpadded(lse_offset) = lse_logsum;
            }
        } else {
            gLSE(tidx / kRowsPerLoadTranspose) = lse_logsum;
        }
    }
    // Store the scales exp(lse - lse_logsum) in shared memory.
    #pragma unroll
    for (int l = 0; l < kNLsePerThread; ++l) {
        const int row = l * kRowsPerLoadTranspose + tidx % kRowsPerLoadTranspose;
        const int col = tidx / kRowsPerLoadTranspose;
        if (row < params.num_splits && col < kBlockM) { sLSE[row][col] = expf(lse_accum(l) - lse_logsum); }
    }
    __syncthreads();

    const index_t row_offset_oaccum = bidx * kBlockM * params.d_rounded;
    Tensor gOaccum = make_tensor(make_gmem_ptr(reinterpret_cast<ElementAccum *>(params.oaccum_ptr) + row_offset_oaccum),
                                 Shape<Int<kBlockM>, Int<kHeadDim>>{},
                                 Stride<Int<kHeadDim>, _1>{});
    constexpr int kBlockN = kNThreads / kBlockM;
    using GmemLayoutAtomOaccum = Layout<Shape<Int<kBlockM>, Int<kBlockN>>, Stride<Int<kBlockN>, _1>>;
    using GmemTiledCopyOaccum = decltype(
        make_tiled_copy(Copy_Atom<DefaultCopy, ElementAccum>{},
                        GmemLayoutAtomOaccum{},
                        Layout<Shape < _1, _4>>{}));  // Val layout, 4 vals per store
    GmemTiledCopyOaccum gmem_tiled_copy_Oaccum;
    auto gmem_thr_copy_Oaccum = gmem_tiled_copy_Oaccum.get_thread_slice(tidx);
    Tensor tOgOaccum = gmem_thr_copy_Oaccum.partition_S(gOaccum);
    Tensor tOrO = make_tensor<ElementAccum>(shape(tOgOaccum));
    Tensor tOrOaccum = make_tensor<ElementAccum>(shape(tOgOaccum));
    clear(tOrO);

    // Predicates
    Tensor cOaccum = make_identity_tensor(Shape<Int<kBlockM>, Int<kHeadDim>>{});
    // Repeat the partitioning with identity layouts
    Tensor tOcOaccum = gmem_thr_copy_Oaccum.partition_S(cOaccum);
    Tensor tOpOaccum = make_tensor<bool>(make_shape(size<2>(tOgOaccum)));
    if (!Is_even_K) {
        #pragma unroll
        for (int k = 0; k < size(tOpOaccum); ++k) { tOpOaccum(k) = get<1>(tOcOaccum(0, 0, k)) < params.d; }
    }
    // Load Oaccum in then scale and accumulate to O
    for (int split = 0; split < params.num_splits; ++split) {
        flash::copy</*Is_even_MN=*/false, Is_even_K>(
            gmem_tiled_copy_Oaccum, tOgOaccum, tOrOaccum, tOcOaccum, tOpOaccum, params.b * params.h * params.seqlen_q - bidx * kBlockM
        );
        #pragma unroll
        for (int m = 0; m < size<1>(tOrOaccum); ++m) {
            int row = get<0>(tOcOaccum(0, m, 0));
            ElementAccum lse_scale = sLSE[split][row];
            #pragma unroll
            for (int k = 0; k < size<2>(tOrOaccum); ++k) {
                #pragma unroll
                for (int i = 0; i < size<0>(tOrOaccum); ++i) {
                    tOrO(i, m, k) += lse_scale * tOrOaccum(i, m, k);
                }
            }
        // if (cute::thread0()) { printf("lse_scale = %f, %f\n", sLSE[split][0], sLSE[split][1]); print(tOrOaccum); }
        }
        tOgOaccum.data() = tOgOaccum.data() + params.b * params.h * params.seqlen_q * params.d_rounded;
    }
    // if (cute::thread0()) { print_tensor(tOrO); }

    Tensor rO = flash::convert_type<Element>(tOrO);
    // Write to gO
    #pragma unroll
    for (int m = 0; m < size<1>(rO); ++m) {
        const int idx = bidx * kBlockM + get<0>(tOcOaccum(0, m, 0));
        if (idx < params.b * params.h * params.seqlen_q) {
            const int batch_idx = idx / (params.h * params.seqlen_q);
            const int head_idx = (idx - batch_idx * (params.h * params.seqlen_q)) / params.seqlen_q;
            // The index to the rows of Q
            const int row = idx - batch_idx * (params.h * params.seqlen_q) - head_idx * params.seqlen_q;
            auto o_ptr = reinterpret_cast<Element *>(params.o_ptr) + batch_idx * params.o_batch_stride
                + head_idx * params.o_head_stride + row * params.o_row_stride;
            #pragma unroll
            for (int k = 0; k < size<2>(rO); ++k) {
                if (Is_even_K || tOpOaccum(k)) {
                    const int col = get<1>(tOcOaccum(0, m, k));
                    Tensor gO = make_tensor(make_gmem_ptr(o_ptr + col),
                                            Shape<Int<decltype(size<0>(rO))::value>>{}, Stride<_1>{});
                    // TODO: Should check if this is using vectorized store, but it seems pretty fast
                    copy(rO(_, m, k), gO);
                    // if (bidx == 0 && tidx == 0) { printf("tidx = %d, idx = %d, batch_idx = %d, head_idx = %d, row = %d, col = %d\n", tidx, idx, batch_idx, head_idx, row, col); print(rO(_, m, k)); print(gO); }
                    // reinterpret_cast<uint64_t *>(o_ptr)[col / 4] = recast<uint64_t>(rO)(0, m, k);
                }
            }
        }
    }
}

} // namespace flash
