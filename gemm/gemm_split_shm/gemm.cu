#include <cuda.h>
#include <stdarg.h>
#include <stdio.h>
#include <cublas_v2.h>
#include <stdlib.h>
#include <cute/tensor.hpp>
#include <float.h>


#define PRINT(name, content) \
    print(name);             \
    print(" : ");            \
    print(content);          \
    print("\n");

#define PRINTTENSOR(name, content) \
    print(name);                   \
    print(" : ");                  \
    print_tensor(content);         \
    print("\n");
    


using T = cute::half_t;
using namespace cute;

#define OFFSET(row, col, ld) ((row) * (ld) + (col))

#define OFFSETCOL(row, col, ld) ((col) * (ld) + (row))

template <typename T>
void cpuF16F16Gemm(T *a, T *b, T *c, int M, int N, int K) {

    for (int m = 0; m < M; m++) {
        for (int n = 0; n < N; n++) {
            float psum = 0.0;
            for (int k = 0; k < K; k++) {
                psum += (float)a[OFFSET(m, k, K)] * (float)b[OFFSETCOL(k, n, K)];
            }
            c[OFFSET(m, n, N)] = (T)psum;
        }
    }
}

template <typename T, int BM, int BM_warp, int BN, int BN_warp, int BK, typename TiledMMA, 
            typename G2SCopyA, typename G2SCopyB,
            typename SmemLayoutA, typename SmemLayoutB, 
            typename S2RCopyAtomA, typename S2RCopyAtomB>
__global__ void gemm_shm_v2(const T *Aptr, const T *Bptr, T *Dptr, int m, int n, int k) {
    // Initilize shared memory
    extern __shared__ T shm_data[];

    T *Ashm = shm_data;
    T *Bshm = shm_data + cute::cosize(SmemLayoutA{});

    // Initilize thread block
    int idx = threadIdx.x;
    int idy = threadIdx.y;
    int ix = blockIdx.x;
    int iy = blockIdx.y;

    
    Tensor A = make_tensor(make_gmem_ptr(Aptr), make_shape(m, k), make_stride(k, Int<1>{}));
    Tensor B = make_tensor(make_gmem_ptr(Bptr), make_shape(n, k), make_stride(k, Int<1>{}));
    Tensor D = make_tensor(make_gmem_ptr(Dptr), make_shape(m, n), make_stride(n, Int<1>{}));

    // Global Memory
    Tensor gA = local_tile(A, make_tile(Int<BM>{}, Int<BK>{}), make_coord(iy, _)); // (BM, BK, num_tile_k)
    Tensor gA_tile = local_tile(gA, make_tile(Int<BM_warp>{}, Int<BK>{}), make_coord(idy, 0, _)); 
    Tensor gB = local_tile(B, make_tile(Int<BN>{}, Int<BK>{}), make_coord(ix, _)); // (BN, BK, num_tile_k)
    Tensor gB_tile = local_tile(gB, make_tile(Int<BN_warp>{}, Int<BK>{}), make_coord(idy, 0, _)); 
    Tensor gD = local_tile(D, make_tile(Int<BM>{}, Int<BN>{}), make_coord(iy, ix)); // (BM, BN) 
    Tensor gD_tile = local_tile(gD, make_tile(Int<BM>{}, Int<BN_warp>{}), make_coord(0, idy)); // (BM_warp, BN_warp)

    // shared memory
    auto sA = make_tensor(make_smem_ptr(Ashm), SmemLayoutA{}); // (kTileM, kTileK)
    auto sA_tile = local_tile(sA, make_tile(Int<BM_warp>{}, Int<BK>{}), make_coord(idy, Int<0>{})); // (BM_warp, BK)
    auto sB = make_tensor(make_smem_ptr(Bshm), SmemLayoutB{}); // (kTileN, kTileK)
    auto sB_tile = local_tile(sB, make_tile(Int<BN_warp>{}, Int<BK>{}), make_coord(idy, Int<0>{})); // (BN_warp, BK)


    // register, use tiled_mma to partition register A/B/C
    TiledMMA tiled_mma;
    auto thr_mma = tiled_mma.get_slice(idx);
    auto tCgD = thr_mma.partition_C(gD_tile); // (MMA, MMA_M, MMA_N)

    auto tCrA = thr_mma.partition_fragment_A(sA(_, _));  // (MMA, MMA_M, MMA_K)
    auto tCrB = thr_mma.partition_fragment_B(sB_tile(_, _));  // (MMA, MMA_N, MMA_K)
    auto tCrD = thr_mma.partition_fragment_C(gD_tile);           // (MMA, MMA_M, MMA_N)
    clear(tCrD);

    // from global memory to shared memory
    G2SCopyA g2s_tiled_copy_a;
    auto g2s_thr_copy_a = g2s_tiled_copy_a.get_slice(idx);
    auto tAgA_copy = g2s_thr_copy_a.partition_S(gA_tile); // (CPY, CPY_M, CPY_K, k)
    auto tAsA_copy = g2s_thr_copy_a.partition_D(sA_tile); // (CPY, CPY_M, CPY_K)

    G2SCopyB g2s_tiled_copy_b;
    auto g2s_thr_copy_b = g2s_tiled_copy_b.get_slice(idx);
    auto tBgB_copy = g2s_thr_copy_b.partition_S(gB_tile); // (CPY, CPY_N, CPY_K, k)
    auto tBsB_copy = g2s_thr_copy_b.partition_D(sB_tile); // (CPY, CPY_N, CPY_K)

    // from shared memory to register, use tiled_mma to generate tiled_copy
    auto s2r_tiled_copy_a = make_tiled_copy_A(S2RCopyAtomA{}, tiled_mma);
    auto s2r_thr_copy_a = s2r_tiled_copy_a.get_slice(idx);
    auto tAsA = s2r_thr_copy_a.partition_S(sA);     // (CPY, CPY_M, CPY_K)
    auto tCrA_view = s2r_thr_copy_a.retile_D(tCrA); // (CPY, CPY_M, CPY_K)

    auto s2r_tiled_copy_b = make_tiled_copy_B(S2RCopyAtomB{}, tiled_mma);
    auto s2r_thr_copy_b = s2r_tiled_copy_b.get_slice(idx);
    auto tBsB = s2r_thr_copy_b.partition_S(sB_tile);     // (CPY, CPY_N, CPY_K)
    auto tCrB_view = s2r_thr_copy_b.retile_D(tCrB); // (CPY, CPY_N, CPY_K)


    // loop over k: i. load tile, ii. mma
    int ntile = k / BK;
    #pragma unroll 1
    for (int itile = 0; itile < ntile; ++itile)
    {
        // copy  (CPY, CPY_M, CPY_K) , async
        cute::copy(g2s_tiled_copy_a, tAgA_copy(_, _, _, itile),
                tAsA_copy(_, _, _));
        cute::copy(g2s_tiled_copy_b, tBgB_copy(_, _, _, itile),
                tBsB_copy(_, _, _));
        cp_async_fence();

        cp_async_wait<0>();
        __syncthreads();

        int nk = size<2>(tCrA);
    #pragma unroll
        for (int ik = 0; ik < nk; ++ik)
        {
            // copy  (CPY, CPY_M), sync
            cute::copy(s2r_tiled_copy_a, tAsA(_, _, ik),
                        tCrA_view(_, _, ik));
            // copy  (CPY, CPY_N)
            cute::copy(s2r_tiled_copy_b, tBsB(_, _, ik),
                        tCrB_view(_, _, ik));
            // (MMA, MMA_M) x (MMA, MMA_N) => (MMA, MMA_M, MMA_N)
            cute::gemm(tiled_mma, tCrD, tCrA(_, _, ik), tCrB(_, _, ik), tCrD);
        } // for ik
    } // itile
    __syncthreads();
    // register to global memory
    cute::copy(tCrD, tCgD);
    __syncthreads();

    if (threadIdx.x == 0 && threadIdx.y == 0 && blockIdx.x == 0 && blockIdx.y == 0)
    {
        printf("### A Tensor ###\n");
        PRINT("gA", gA.shape())     
        PRINT("gA_tile", gA_tile.shape())
        PRINT("tAgA_copy", tAgA_copy.shape())
        PRINT("sA", sA.shape())
        PRINT("sA_tile", sA_tile.shape())
        PRINT("tAsA_copy", tAsA_copy.shape())
        PRINT("tCrA", tCrA.shape()) 
        PRINT("tCrA_view", tCrA_view.layout()) 

        printf("### B Tensor ###\n");
        PRINT("gB", gB.shape())     
        PRINT("gB_tile", gB_tile.shape())
        PRINT("tBgB_copy", tBgB_copy.shape())
        PRINT("sB", sB.shape())
        PRINT("tBsB_copy", tBsB_copy.shape())
        PRINT("sB_tile", sB_tile.shape())
        PRINT("tCrB", tCrB.shape()) 
        PRINT("tCrB_view", tCrB_view.layout()) 

        printf("### D Tensor ###\n");
        PRINT("gD", gD.shape())
        PRINT("gD_tile", gD_tile.shape())
    }
}

template <typename T>
void gemm_v2(T *a, T *b, T *c, int M, int N, int K) {

    auto BM = Int<128>{};
    auto BN = Int<128>{};
    auto BK = Int< 32>{};

    const int warp_y = 4;
    const int BM_warp = BM / warp_y;
    const int BN_warp = BN / warp_y;

    // Define the smem layouts
    using SmemLayoutAtom = decltype(composition(
        Swizzle<3, 3, 3>{},
        make_layout(make_shape(Int<8>{}, Int<BK>{}),
                    make_stride(Int<BK>{}, Int<1>{}))));
    using SmemLayoutA = decltype(tile_to_shape(SmemLayoutAtom{},
                                               make_shape(Int<BM>{}, Int<BK>{})));
    using SmemLayoutB = decltype(tile_to_shape(SmemLayoutAtom{},
                                               make_shape(Int<BN>{}, Int<BK>{})));                    // (m,n) -> smem_idx
    
    // mma
    using mma_op = SM80_16x8x16_F16F16F16F16_TN;
    using mma_traits = MMA_Traits<mma_op>;
    using mma_atom = MMA_Atom<mma_traits>;
    static constexpr int kMmaEURepeatM = 1;
    static constexpr int kMmaEURepeatN = 1;
    static constexpr int kMmaEURepeatK = 1;

    using mma_atom_shape = mma_traits::Shape_MNK;
    static constexpr int kMmaPM = 1 * kMmaEURepeatM * get<0>(mma_atom_shape{});
    static constexpr int kMmaPN = 2 * kMmaEURepeatN * get<1>(mma_atom_shape{});
    static constexpr int kMmaPK = 1 * kMmaEURepeatK * get<2>(mma_atom_shape{});
    using MMA_EU_RepeatT = decltype(make_layout(make_shape(
        Int<kMmaEURepeatM>{}, Int<kMmaEURepeatN>{}, Int<kMmaEURepeatK>{})));
    using MMA_P_T = Tile<Int<kMmaPM>, Int<kMmaPN>, Int<kMmaPK>>;
  
    using MMA = decltype(make_tiled_mma(mma_atom{}, MMA_EU_RepeatT{}, MMA_P_T{}));

    // copy from global memory to shared memory
    using g2s_copy_op = SM80_CP_ASYNC_CACHEGLOBAL<cute::uint128_t>;
    using g2s_copy_traits = Copy_Traits<g2s_copy_op>;
    using g2s_copy_atom = Copy_Atom<g2s_copy_traits, T>;
    using G2SCopyA =
        decltype(make_tiled_copy(g2s_copy_atom{},
                                 make_layout(make_shape(Int<8>{}, Int<4>{}), // Thr layout 32x4 k-major
                                             make_stride(Int<4>{}, Int<1>{})),
                                 make_layout(make_shape(Int<1>{}, Int<8>{})))); // Val layout 1x8
    using G2SCopyB = G2SCopyA;

    // copy from shared memory to register
    // use mma tiled ,so no tiled here
    using s2r_copy_op = SM75_U32x4_LDSM_N;
    using s2r_copy_traits = Copy_Traits<s2r_copy_op>;
    using s2r_copy_atom = Copy_Atom<s2r_copy_traits, T>;
    using S2RCopyAtomA = s2r_copy_atom;
    using S2RCopyAtomB = s2r_copy_atom;

    int BX = (N + BN - 1) / BN;
    int BY = (M + BM - 1) / BM;
    

    dim3 block(size(MMA{}), warp_y, 1);
    dim3 grid(BX, BY);

    // C_shm is shared with A_shm and B_shm
    static constexpr int shm_size_AB = cute::cosize(SmemLayoutA{}) + cute::cosize(SmemLayoutB{});
    static constexpr int kShmSize    = shm_size_AB * sizeof(T);

    int shm_size = kShmSize;

    cudaFuncSetAttribute(gemm_shm_v2<T, BM, BM_warp, BN, BN_warp, BK, MMA, G2SCopyA, G2SCopyB, SmemLayoutA, SmemLayoutB, S2RCopyAtomA, S2RCopyAtomB>,
                         cudaFuncAttributeMaxDynamicSharedMemorySize, shm_size);
    
    gemm_shm_v2<T, BM, BM_warp, BN, BN_warp, BK, MMA, G2SCopyA, G2SCopyB, SmemLayoutA, SmemLayoutB, S2RCopyAtomA, S2RCopyAtomB>
               <<<grid, block, shm_size>>>(a, b, c, M, N, K);
}


template <typename T>
float testF16F16GemmMaxError(
    void (*gpuF16F16Gemm) (T *, T *, T *, int, int, int),
    int M, int N, int K) {

    size_t size_a = M * K * sizeof(T);
    size_t size_b = K * N * sizeof(T);
    size_t size_c = M * N * sizeof(T);

    T *h_a, *h_b, *d_a, *d_b;
    T *h_c, *d_c, *h_d_c;

    h_a = (T *)malloc(size_a);
    h_b = (T *)malloc(size_b);
    h_c = (T *)malloc(size_c);
    cudaMalloc(&d_a, size_a);
    cudaMalloc(&d_b, size_b);
    cudaMalloc(&d_c, size_c);

    h_d_c = (T *)malloc(size_c);

    srand(time(0));
    for (int i = 0; i < M * K; i++)
        h_a[i] = (T)(rand() / float(RAND_MAX));
    for (int i = 0; i < K * N; i++)
        h_b[i] = (T)(rand() / float(RAND_MAX));

    cpuF16F16Gemm(h_a, h_b, h_c, M, N, K);

    cudaMemcpy(d_a, h_a, size_a, cudaMemcpyHostToDevice);
    cudaMemcpy(d_b, h_b, size_b, cudaMemcpyHostToDevice);

    gpuF16F16Gemm(d_a, d_b, d_c, M, N, K);

    cudaMemcpy(h_d_c, d_c, size_c, cudaMemcpyDeviceToHost);

    float max_error = 0.0;
    for (int i = 0; i < M * N; i++) {
        float this_error = abs((float)h_d_c[i] - (float)h_c[i]);
        if (max_error != max_error || this_error != this_error) // nan
            max_error = -NAN;
        else
            max_error = max(max_error, this_error);
    }

    free(h_a); free(h_b); free(h_c); 
    cudaFree(d_a); cudaFree(d_b); cudaFree(d_c); free(h_d_c);

    return max_error;
}

template <typename T>
float testF16F16GemmPerformance(
    void (*gpuF16F16Gemm) (T *, T *, T *, int, int, int),
    int M, int N, int K, int repeat) {

    size_t size_a = M * K * sizeof(T);
    size_t size_b = K * N * sizeof(T);
    size_t size_c = M * N * sizeof(T);

    T *d_a, *d_b;
    T *d_c;
    cudaMalloc(&d_a, size_a);
    cudaMalloc(&d_b, size_b);
    cudaMalloc(&d_c, size_c);

    cudaEvent_t start, end;
    cudaEventCreate(&start);
    cudaEventCreate(&end);
    cudaEventRecord(start);
    for (int i = 0; i < repeat; i++) {
        gpuF16F16Gemm(d_a, d_b, d_c, M, N, K);
    }
    cudaEventRecord(end);
    cudaEventSynchronize(end);

    float msec, sec;
    cudaEventElapsedTime(&msec, start, end);
    sec = msec / 1000.0 / repeat;

    cudaFree(d_a);
    cudaFree(d_b);
    cudaFree(d_c);
    cudaEventDestroy(start);
    cudaEventDestroy(end);

    return sec;
}


int main() {
    const int test_num = 64;
    int M_list[test_num];
    int N_list[test_num];
    int K_list[test_num];

    for (int i = 0; i < test_num; i++) {
        M_list[i] = (i + 1) * 256;
        N_list[i] = (i + 1) * 256;
        K_list[i] = (i + 1) * 256;
    }

    const int outer_repeat = 10, inner_repeat = 1;

    printf("\nalgo = Cute_HGEMM_V2\n");

    const int M = 1024, N = 1024, K = 1024;
    float max_error = testF16F16GemmMaxError<T>(
        gemm_v2, M, N, K);
    printf("Max Error = %f\n", max_error);

    // double this_sec = testF16F16GemmPerformance<T>(
    //     gemm_v2, 8192, 8192, 8192, inner_repeat);
    // for (int j = 0; j < test_num; j++) {
    //     int M = M_list[j], N = N_list[j], K = K_list[j];

    //     double max_sec = 0.0;
    //     double min_sec = DBL_MAX;
    //     double total_sec = 0.0;

    //     for (int k = 0; k < outer_repeat; k++) {
    //         double this_sec = testF16F16GemmPerformance<T>(
    //             gemm_v2, M, N, K, inner_repeat);
    //         max_sec = max(max_sec, this_sec);
    //         min_sec = min(min_sec, this_sec);
    //         total_sec += this_sec;
    //     }

    //     double avg_sec = total_sec / outer_repeat;
    //     double avg_Gflops = ((double)M) * N * K * 2 / 1024 / 1024 / 1024 / avg_sec;

    //     printf("M N K = %6d %6d %6d, ", M, N, K);
    //     printf("Time = %12.8lf %12.8lf %12.8lf s, ", min_sec, avg_sec, max_sec);
    //     printf("AVG Performance = %10.4lf Gflops\n", avg_Gflops);
    // }

    return 0;
}