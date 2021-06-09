// progpow: C/C++ implementation of ProgPow.
// Copyright 2018-2019 Pawel Bylica.
// Licensed under the Apache License, Version 2.0.

// Modified by Firominer's authors 2021

#include "progpow.hpp"

#include <sstream>

#define rnd() (kiss99(rnd_state))
#define mix_src() ("mix[" + std::to_string(rnd() % kRegs) + "]")
#define mix_dst() ("mix[" + std::to_string(mix_seq_dst[(mix_seq_dst_cnt++) % kRegs]) + "]")
#define mix_cache() \
    ("mix[" + std::to_string(mix_seq_cache[(mix_seq_cache_cnt++) % kRegs]) + "]")

namespace progpow
{
void swap(int& a, int& b)
{
    int t = a;
    a = b;
    b = t;
}

static inline uint32_t fnv1a(uint32_t& h, uint32_t d)
{
    return h = static_cast<uint32_t>(static_cast<uint64_t>(h ^ d) * 0x1000193u);
}

// KISS99 is simple, fast, and passes the TestU01 suite
// https://en.wikipedia.org/wiki/KISS_(algorithm)
// http://www.cse.yorku.ca/~oz/marsaglia-rng.html
static uint32_t kiss99(kiss99_t& st)
{
    st.z = 36969 * (st.z & 65535) + (st.z >> 16);
    st.w = 18000 * (st.w & 65535) + (st.w >> 16);
    uint32_t MWC = ((st.z << 16) + st.w);
    st.jsr ^= (st.jsr << 17);
    st.jsr ^= (st.jsr >> 13);
    st.jsr ^= (st.jsr << 5);
    st.jcong = 69069 * st.jcong + 1234567;
    return ((MWC ^ st.jcong) + st.jsr);
}

// Merge new data from b into the value in a
// Assuming A has high entropy only do ops that retain entropy, even if B is low entropy
// (IE don't do A&B)
static std::string merge(std::string a, std::string b, uint32_t r)
{
    switch (r % 4)
    {
    case 0:
        return a + " = (" + a + " * 33) + " + b + ";\n";
    case 1:
        return a + " = (" + a + " ^ " + b + ") * 33;\n";
    case 2:
        return a + " = ROTL32(" + a + ", " + std::to_string(((r >> 16) % 31) + 1) + ") ^ " + b +
               ";\n";
    case 3:
        return a + " = ROTR32(" + a + ", " + std::to_string(((r >> 16) % 31) + 1) + ") ^ " + b +
               ";\n";
    }
    return "#error\n";
}

// Random math between two input values
static std::string math(std::string d, std::string a, std::string b, uint32_t r)
{
    switch (r % 11)
    {
    case 0:
        return d + " = " + a + " + " + b + ";\n";
    case 1:
        return d + " = " + a + " * " + b + ";\n";
    case 2:
        return d + " = mul_hi(" + a + ", " + b + ");\n";
    case 3:
        return d + " = min(" + a + ", " + b + ");\n";
    case 4:
        return d + " = ROTL32(" + a + ", " + b + " % 32);\n";
    case 5:
        return d + " = ROTR32(" + a + ", " + b + " % 32);\n";
    case 6:
        return d + " = " + a + " & " + b + ";\n";
    case 7:
        return d + " = " + a + " | " + b + ";\n";
    case 8:
        return d + " = " + a + " ^ " + b + ";\n";
    case 9:
        return d + " = clz(" + a + ") + clz(" + b + ");\n";
    case 10:
        return d + " = popcount(" + a + ") + popcount(" + b + ");\n";
    }
    return "#error\n";
}

std::string getKern(uint64_t prog_seed, kernel_type kern)
{
    std::stringstream ret;

    uint32_t seed0 = static_cast<uint32_t>(prog_seed);
    uint32_t seed1 = static_cast<uint32_t>(prog_seed >> 32);
    uint32_t fnv_hash = 0x811c9dc5;
    kiss99_t rnd_state;
    rnd_state.z = fnv1a(fnv_hash, seed0);
    rnd_state.w = fnv1a(fnv_hash, seed1);
    rnd_state.jsr = fnv1a(fnv_hash, seed0);
    rnd_state.jcong = fnv1a(fnv_hash, seed1);

    // Create a random sequence of mix destinations and cache sources
    // Merge is a read-modify-write, guaranteeing every mix element is modified every loop
    // Guarantee no cache load is duplicated and can be optimized away
    int mix_seq_dst[kRegs];
    int mix_seq_cache[kRegs];
    int mix_seq_dst_cnt = 0;
    int mix_seq_cache_cnt = 0;
    for (int i = 0; i < kRegs; i++)
    {
        mix_seq_dst[i] = i;
        mix_seq_cache[i] = i;
    }
    for (int i = kRegs - 1; i > 0; i--)
    {
        int j;
        j = rnd() % (i + 1);
        swap(mix_seq_dst[i], mix_seq_dst[j]);
        j = rnd() % (i + 1);
        swap(mix_seq_cache[i], mix_seq_cache[j]);
    }

    if (kern == kernel_type::Cuda)
    {
        ret << "typedef unsigned int       uint32_t;\n";
        ret << "typedef unsigned long long uint64_t;\n";
        ret << "#if __CUDA_ARCH__ < 350\n";
        ret << "#define ROTL32(x,n) (((x) << (n % 32)) | ((x) >> (32 - (n % 32))))\n";
        ret << "#define ROTR32(x,n) (((x) >> (n % 32)) | ((x) << (32 - (n % 32))))\n";
        ret << "#else\n";
        ret << "#define ROTL32(x,n) __funnelshift_l((x), (x), (n))\n";
        ret << "#define ROTR32(x,n) __funnelshift_r((x), (x), (n))\n";
        ret << "#endif\n";
        ret << "#define min(a,b) ((a<b) ? a : b)\n";
        ret << "#define mul_hi(a, b) __umulhi(a, b)\n";
        ret << "#define clz(a) __clz(a)\n";
        ret << "#define popcount(a) __popc(a)\n\n";

        ret << "#define DEV_INLINE __device__ __forceinline__\n";
        ret << "#if (__CUDACC_VER_MAJOR__ > 8)\n";
        ret << "#define SHFL(x, y, z) __shfl_sync(0xFFFFFFFF, (x), (y), (z))\n";
        ret << "#else\n";
        ret << "#define SHFL(x, y, z) __shfl((x), (y), (z))\n";
        ret << "#endif\n\n";

        ret << "\n";
    }
    else
    {
        ret << "#ifndef GROUP_SIZE\n";
        ret << "#define GROUP_SIZE 128\n";
        ret << "#endif\n";
        ret << "#define GROUP_SHARE (GROUP_SIZE / " << kLanes << ")\n";
        ret << "\n";
        ret << "typedef unsigned int       uint32_t;\n";
        ret << "typedef unsigned long      uint64_t;\n";
        ret << "#define ROTL32(x, n) rotate((x), (uint32_t)(n))\n";
        ret << "#define ROTR32(x, n) rotate((x), (uint32_t)(32-n))\n";
        ret << "\n";
    }

    ret << "#define PROGPOW_LANES           " << kLanes << "\n";
    ret << "#define PROGPOW_REGS            " << kRegs << "\n";
    ret << "#define PROGPOW_DAG_LOADS       " << kDag_loads << "\n";
    ret << "#define PROGPOW_CACHE_WORDS     " << kCache_bytes / sizeof(uint32_t) << "\n";
    ret << "#define PROGPOW_CNT_DAG         " << kDag_count << "\n";
    ret << "#define PROGPOW_CNT_MATH        " << kMath_count << "\n";
    ret << "\n";

    if (kern == kernel_type::Cuda)
    {
        ret << "typedef struct __align__(16) {uint32_t s[PROGPOW_DAG_LOADS];} dag_t;\n";
        ret << "\n";
        ret << "// Inner loop for prog_seed " << prog_seed << "\n";
        ret << "__device__ __forceinline__ void progPowLoop(const uint32_t loop,\n";
        ret << "        uint32_t mix[PROGPOW_REGS],\n";
        ret << "        const dag_t *g_dag,\n";
        ret << "        const uint32_t c_dag[PROGPOW_CACHE_WORDS],\n";
        ret << "        const bool hack_false)\n";
    }
    else
    {
        ret << "typedef struct __attribute__ ((aligned (16))) {uint32_t s[PROGPOW_DAG_LOADS];} "
               "dag_t;\n";
        ret << "\n";
        ret << "// Inner loop for prog_seed " << prog_seed << "\n";
        ret << "inline void progPowLoop(const uint32_t loop,\n";
        ret << "        volatile uint32_t mix_arg[PROGPOW_REGS],\n";
        ret << "        __global const dag_t *g_dag,\n";
        ret << "        __local const uint32_t c_dag[PROGPOW_CACHE_WORDS],\n";
        ret << "        __local uint64_t share[GROUP_SHARE],\n";
        ret << "        const bool hack_false)\n";
    }
    ret << "{\n";

    ret << "dag_t data_dag;\n";
    ret << "uint32_t offset, data;\n";
    // Work around AMD OpenCL compiler bug
    // See https://github.com/gangnamtestnet/firominer/issues/16
    if (kern == kernel_type::OpenCL)
    {
        ret << "uint32_t mix[PROGPOW_REGS];\n";
        ret << "for(int i=0; i<PROGPOW_REGS; i++)\n";
        ret << "    mix[i] = mix_arg[i];\n";
    }

    if (kern == kernel_type::Cuda)
        ret << "const uint32_t lane_id = threadIdx.x & (PROGPOW_LANES-1);\n";
    else
    {
        ret << "const uint32_t lane_id = get_local_id(0) & (PROGPOW_LANES-1);\n";
        ret << "const uint32_t group_id = get_local_id(0) / PROGPOW_LANES;\n";
    }

    // Global memory access
    // lanes access sequential locations
    // Hard code mix[0] to guarantee the address for the global load depends on the result of the
    // load
    ret << "// global load\n";
    if (kern == kernel_type::Cuda)
        ret << "offset = SHFL(mix[0], loop%PROGPOW_LANES, PROGPOW_LANES);\n";
    else
    {
        ret << "if(lane_id == (loop % PROGPOW_LANES))\n";
        ret << "    share[group_id] = mix[0];\n";
        ret << "barrier(CLK_LOCAL_MEM_FENCE);\n";
        ret << "offset = share[group_id];\n";
    }
    ret << "offset %= PROGPOW_DAG_ELEMENTS;\n";
    ret << "offset = offset * PROGPOW_LANES + (lane_id ^ loop) % PROGPOW_LANES;\n";
    ret << "data_dag = g_dag[offset];\n";
    ret << "// hack to prevent compiler from reordering LD and usage\n";
    if (kern == kernel_type::Cuda)
        ret << "if (hack_false) __threadfence_block();\n";
    else
        ret << "if (hack_false) barrier(CLK_LOCAL_MEM_FENCE);\n";

    for (int i = 0; (i < kCache_count) || (i < kMath_count); i++)
    {
        if (i < kCache_count)
        {
            // Cached memory access
            // lanes access random locations
            std::string src = mix_cache();
            std::string dest = mix_dst();
            uint32_t r = rnd();
            ret << "// cache load " << i << "\n";
            ret << "offset = " << src << " % PROGPOW_CACHE_WORDS;\n";
            ret << "data = c_dag[offset];\n";
            ret << merge(dest, "data", r);
        }
        if (i < kMath_count)
        {
            // Random Math
            // Generate 2 unique sources
            int src_rnd = rnd() % ((kRegs - 1) * kRegs);
            int src1 = src_rnd % kRegs;  // 0 <= src1 < kRegs
            int src2 = src_rnd / kRegs;  // 0 <= src2 < kRegs - 1
            if (src2 >= src1)
                ++src2;  // src2 is now any reg other than src1
            std::string src1_str = "mix[" + std::to_string(src1) + "]";
            std::string src2_str = "mix[" + std::to_string(src2) + "]";
            uint32_t r1 = rnd();
            std::string dest = mix_dst();
            uint32_t r2 = rnd();
            ret << "// random math " << i << "\n";
            ret << math("data", src1_str, src2_str, r1);
            ret << merge(dest, "data", r2);
        }
    }
    // Consume the global load data at the very end of the loop, to allow fully latency hiding
    ret << "// consume global load data\n";
    ret << "// hack to prevent compiler from reordering LD and usage\n";
    if (kern == kernel_type::Cuda)
        ret << "if (hack_false) __threadfence_block();\n";
    else
        ret << "if (hack_false) barrier(CLK_LOCAL_MEM_FENCE);\n";
    ret << merge("mix[0]", "data_dag.s[0]", rnd());
    for (int i = 1; i < kDag_loads; i++)
    {
        std::string dest = mix_dst();
        uint32_t r = rnd();
        ret << merge(dest, "data_dag.s[" + std::to_string(i) + "]", r);
    }

    // Work around AMD OpenCL compiler bug
    if (kern == kernel_type::OpenCL)
    {
        ret << "for(int i=0; i<PROGPOW_REGS; i++)\n";
        ret << "    mix_arg[i] = mix[i];\n";
    }
    ret << "}\n";
    ret << "\n";

    return ret.str();
}


}  // namespace progpow