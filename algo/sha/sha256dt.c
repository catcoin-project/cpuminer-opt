#include "algo-gate-api.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "sha256-hash.h"

#if defined(__AVX512F__) && defined(__AVX512VL__) && defined(__AVX512DQ__) && defined(__AVX512BW__)
  #define SHA256DT_16WAY 1
#elif defined(__SHA__)
  #define SHA256DT_SHA 1
#elif defined(__AVX2__)
  #define SHA256DT_8WAY 1
#else
  #define SHA256DT_4WAY 1
#endif

static const uint32_t sha256dt_iv[8]  __attribute__ ((aligned (32))) =
   {
      0xdfa9bf2c, 0xb72074d4, 0x6bb01122, 0xd338e869,
      0xaa3ff126, 0x475bbf30, 0x8fd52e5b, 0x9f75c9ad
   };

#if defined(SHA256DT_SHA)

int scanhash_sha256dt_sha( struct work *work, uint32_t max_nonce,
                      uint64_t *hashes_done, struct thr_info *mythr )
{
   uint32_t block1a[16] __attribute__ ((aligned (64)));
   uint32_t block1b[16] __attribute__ ((aligned (64)));
   uint32_t block2a[16] __attribute__ ((aligned (64)));
   uint32_t block2b[16] __attribute__ ((aligned (64)));
   uint32_t hasha[8]    __attribute__ ((aligned (32)));
   uint32_t hashb[8]    __attribute__ ((aligned (32)));
   uint32_t mstatea[8]  __attribute__ ((aligned (32)));
   uint32_t mstateb[8]  __attribute__ ((aligned (32)));
   uint32_t sstate[8]   __attribute__ ((aligned (32)));
   uint32_t *pdata = work->data;
   uint32_t *ptarget = work->target;
   const uint32_t first_nonce = pdata[19];
   const uint32_t last_nonce = max_nonce - 2;
   uint32_t n = first_nonce;
   const int thr_id = mythr->id;
   const bool bench = opt_benchmark;
   const __m128i shuf_bswap32 =
           _mm_set_epi64x( 0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL );

   // hash first 64 byte block of data
   sha256_opt_transform_le( mstatea, pdata, sha256dt_iv );

   // fill & pad second bock without nonce
   memcpy( block1a, pdata + 16, 12 );
   memcpy( block1b, pdata + 16, 12 );
   block1a[ 3] = block1b[ 3] = 0;
   block1a[ 4] = block1b[ 4] = 0x80000000;
   memset( block1a + 5, 0, 40 );
   memset( block1b + 5, 0, 40 );
   block1a[15] = block1b[15] = 0x480; // funky bit count

   sha256_ni_prehash_3rounds( mstateb, block1a, sstate, mstatea);

   // Pad third block
   block2a[ 8] = block2b[ 8] = 0x80000000;
   memset( block2a + 9, 0, 24 );
   memset( block2b + 9, 0, 24 );
   block2a[15] = block2b[15] = 0x300; // bit count

   do
   {
      // Insert nonce for second block
      block1a[3] = n;
      block1b[3] = n+1;
      sha256_ni2way_final_rounds( block2a, block2b, block1a, block1b,
                                  mstateb, mstateb, sstate, sstate );

      sha256_ni2way_transform_le( hasha, hashb, block2a, block2b,
                                  sha256dt_iv, sha256dt_iv );

      if ( unlikely( bswap_32( hasha[7] ) <= ptarget[7] ) )
      {
          casti_m128i( hasha, 0 ) =
               _mm_shuffle_epi8( casti_m128i( hasha, 0 ), shuf_bswap32 );
          casti_m128i( hasha, 1 ) =
               _mm_shuffle_epi8( casti_m128i( hasha, 1 ), shuf_bswap32 );
          if ( likely( valid_hash( hasha, ptarget ) && !bench ) )
          {
             pdata[19] = n;
             submit_solution( work, hasha, mythr );
          }
      }
      if ( unlikely( bswap_32( hashb[7] ) <= ptarget[7] ) )
      {
         casti_m128i( hashb, 0 ) =
               _mm_shuffle_epi8( casti_m128i( hashb, 0 ), shuf_bswap32 );
         casti_m128i( hashb, 1 ) =
               _mm_shuffle_epi8( casti_m128i( hashb, 1 ), shuf_bswap32 );
         if ( likely( valid_hash( hashb, ptarget ) && !bench ) )
         {
            pdata[19] = n+1;
            submit_solution( work, hashb, mythr );
         }
      }
      n += 2;
   } while ( (n < last_nonce) && !work_restart[thr_id].restart );

   pdata[19] = n;
   *hashes_done = n - first_nonce;
   return 0;
}

#endif

#if defined(SHA256DT_16WAY)

int scanhash_sha256dt_16way( struct work *work, const uint32_t max_nonce,
                           uint64_t *hashes_done, struct thr_info *mythr )
{
   __m512i  block[16]    __attribute__ ((aligned (128)));
   __m512i  buf[16]      __attribute__ ((aligned (64)));
   __m512i  hash32[8]    __attribute__ ((aligned (64)));
   __m512i  mstate1[8]   __attribute__ ((aligned (64)));
   __m512i  mstate2[8]   __attribute__ ((aligned (64)));
   __m512i  istate[8]    __attribute__ ((aligned (64)));
   __m512i  mexp_pre[8]  __attribute__ ((aligned (64)));
   uint32_t phash[8]     __attribute__ ((aligned (32)));
   uint32_t *pdata = work->data;
   const uint32_t *ptarget = work->target;
   const uint32_t first_nonce = pdata[19];
   const uint32_t last_nonce = max_nonce - 16;
   const __m512i last_byte = v512_32( 0x80000000 );
   uint32_t n = first_nonce;
   const int thr_id = mythr->id;
   const __m512i sixteen = v512_32( 16 );
   const bool bench = opt_benchmark;
   const __m256i bswap_shuf = mm256_bcast_m128( _mm_set_epi64x(
                                0x0c0d0e0f08090a0b, 0x0405060700010203 ) );

   // prehash first block directly from pdata
   sha256_transform_le( phash, pdata, sha256dt_iv );

   // vectorize block 0 hash for second block
   mstate1[0] = v512_32( phash[0] );
   mstate1[1] = v512_32( phash[1] );
   mstate1[2] = v512_32( phash[2] );
   mstate1[3] = v512_32( phash[3] );
   mstate1[4] = v512_32( phash[4] );
   mstate1[5] = v512_32( phash[5] );
   mstate1[6] = v512_32( phash[6] );
   mstate1[7] = v512_32( phash[7] );

   // second message block data, with nonce & padding
   buf[0] = v512_32( pdata[16] );
   buf[1] = v512_32( pdata[17] );
   buf[2] = v512_32( pdata[18] );
   buf[3] = _mm512_set_epi32( n+15, n+14, n+13, n+12, n+11, n+10, n+ 9, n+ 8,
                              n+ 7, n+ 6, n+ 5, n+ 4, n+ 3, n+ 2, n +1, n );
   buf[4] = last_byte;
   memset_zero_512( buf+5, 10 );
   buf[15] = v512_32( 0x480 ); // sha256dt funky bit count

   // partially pre-expand & prehash second message block, avoiding the nonces
   sha256_16way_prehash_3rounds( mstate2, mexp_pre, buf, mstate1 );

   // vectorize IV for second hash
   istate[0] = v512_32( sha256dt_iv[0] );
   istate[1] = v512_32( sha256dt_iv[1] );
   istate[2] = v512_32( sha256dt_iv[2] );
   istate[3] = v512_32( sha256dt_iv[3] );
   istate[4] = v512_32( sha256dt_iv[4] );
   istate[5] = v512_32( sha256dt_iv[5] );
   istate[6] = v512_32( sha256dt_iv[6] );
   istate[7] = v512_32( sha256dt_iv[7] );

   // initialize padding for second hash
   block[ 8] = last_byte;
   memset_zero_512( block+9, 6 );
   block[15] = v512_32( 0x300 ); // bit count

   do
   {
      sha256_16way_final_rounds( block, buf, mstate1, mstate2, mexp_pre );
      if ( unlikely( sha256_16way_transform_le_short(
                                  hash32, block, istate, ptarget ) ) )
      {
         for ( int lane = 0; lane < 16; lane++ )
         {
            extr_lane_16x32( phash, hash32, lane, 256 );
            casti_m256i( phash, 0 ) =
                   _mm256_shuffle_epi8( casti_m256i( phash, 0 ), bswap_shuf ); 
            if ( likely( valid_hash( phash, ptarget ) && !bench ) )
            {
              pdata[19] = n + lane;
              submit_solution( work, phash, mythr );
            }
         }
      }
      buf[3] = _mm512_add_epi32( buf[3], sixteen );
      n += 16;
   } while ( (n < last_nonce) && !work_restart[thr_id].restart );
   pdata[19] = n;
   *hashes_done = n - first_nonce;
   return 0;
}
   
#endif

#if defined(SHA256DT_8WAY)

int scanhash_sha256dt_8way( struct work *work, const uint32_t max_nonce,
                           uint64_t *hashes_done, struct thr_info *mythr )
{
   __m256i  vdata[32]    __attribute__ ((aligned (64)));
   __m256i  block[16]    __attribute__ ((aligned (32)));
   __m256i  hash32[8]    __attribute__ ((aligned (32)));
   __m256i  istate[8]    __attribute__ ((aligned (32)));
   __m256i  mstate1[8]   __attribute__ ((aligned (32)));
   __m256i  mstate2[8]   __attribute__ ((aligned (32)));
   __m256i  mexp_pre[8]  __attribute__ ((aligned (32)));
   uint32_t lane_hash[8] __attribute__ ((aligned (32)));
   uint32_t *pdata = work->data;
   const uint32_t *ptarget = work->target;
   const uint32_t first_nonce = pdata[19];
   const uint32_t last_nonce = max_nonce - 8;
   uint32_t n = first_nonce;
   __m256i *noncev = vdata + 19;
   const int thr_id = mythr->id;
   const bool bench = opt_benchmark;
   const __m256i last_byte = v256_32( 0x80000000 );
   const __m256i eight = v256_32( 8 );
   const __m256i bswap_shuf = mm256_bcast_m128( _mm_set_epi64x(
                                0x0c0d0e0f08090a0b, 0x0405060700010203 ) );

   for ( int i = 0; i < 19; i++ )
      vdata[i] = v256_32( pdata[i] );

   *noncev = _mm256_set_epi32( n+ 7, n+ 6, n+ 5, n+ 4, n+ 3, n+ 2, n+1, n );

   vdata[16+4] = last_byte;
   memset_zero_256( vdata+16 + 5, 10 );
   vdata[16+15] = v256_32( 0x480 );

   block[ 8] = last_byte;
   memset_zero_256( block + 9, 6 );
   block[15] = v256_32( 0x300 ); 
   
   // initialize state for second hash
   istate[0] = v256_32( sha256dt_iv[0] );
   istate[1] = v256_32( sha256dt_iv[1] );
   istate[2] = v256_32( sha256dt_iv[2] );
   istate[3] = v256_32( sha256dt_iv[3] );
   istate[4] = v256_32( sha256dt_iv[4] );
   istate[5] = v256_32( sha256dt_iv[5] );
   istate[6] = v256_32( sha256dt_iv[6] );
   istate[7] = v256_32( sha256dt_iv[7] );

   sha256_8way_transform_le( mstate1, vdata, istate );

   // Do 3 rounds on the first 12 bytes of the next block
   sha256_8way_prehash_3rounds( mstate2, mexp_pre, vdata + 16, mstate1 );
   
   do
   {
      sha256_8way_final_rounds( block, vdata+16, mstate1, mstate2, mexp_pre );
      if ( unlikely( sha256_8way_transform_le_short( hash32, block,
                                                     istate, ptarget ) ) )
      {
         for ( int lane = 0; lane < 8; lane++ )
         {
            extr_lane_8x32( lane_hash, hash32, lane, 256 );
            casti_m256i( lane_hash, 0 ) =
               _mm256_shuffle_epi8( casti_m256i( lane_hash, 0 ), bswap_shuf );
            if ( likely( valid_hash( lane_hash, ptarget ) && !bench ) )
            {
               pdata[19] = n + lane;
               submit_solution( work, lane_hash, mythr );
            }
         }
      }
      *noncev = _mm256_add_epi32( *noncev, eight );
      n += 8;
   } while ( (n < last_nonce) && !work_restart[thr_id].restart );
   pdata[19] = n;
   *hashes_done = n - first_nonce;
   return 0;
}

#endif

#if defined(SHA256DT_4WAY)

int scanhash_sha256dt_4way( struct work *work, const uint32_t max_nonce,
                           uint64_t *hashes_done, struct thr_info *mythr )
{
   __m128i  vdata[32]    __attribute__ ((aligned (64)));
   __m128i  block[16]    __attribute__ ((aligned (32)));
   __m128i  hash32[8]    __attribute__ ((aligned (32)));
   __m128i  initstate[8] __attribute__ ((aligned (32)));
   __m128i  midstate[8]  __attribute__ ((aligned (32)));
   uint32_t lane_hash[8] __attribute__ ((aligned (32)));
   uint32_t *hash32_d7 =  (uint32_t*)&( hash32[7] );
   uint32_t *pdata = work->data;
   const uint32_t *ptarget = work->target;
   const uint32_t targ32_d7 = ptarget[7];
   const uint32_t first_nonce = pdata[19];
   const uint32_t last_nonce = max_nonce - 4;
   uint32_t n = first_nonce;
   __m128i *noncev = vdata + 19;
   const int thr_id = mythr->id;
   const bool bench = opt_benchmark;
   const __m128i last_byte = v128_32( 0x80000000 );
   const __m128i four = v128_32( 4 );

   for ( int i = 0; i < 19; i++ )
       vdata[i] = v128_32( pdata[i] );

   *noncev = _mm_set_epi32( n+ 3, n+ 2, n+1, n );

   vdata[16+4] = last_byte;
   memset_zero_128( vdata+16 + 5, 10 );
   vdata[16+15] = v128_32( 0x480 );

   block[ 8] = last_byte;
   memset_zero_128( block + 9, 6 );
   block[15] = v128_32( 0x300 );
   
   // initialize state
   initstate[0] = v128_32( sha256dt_iv[0] );
   initstate[1] = v128_32( sha256dt_iv[1] );
   initstate[2] = v128_32( sha256dt_iv[2] );
   initstate[3] = v128_32( sha256dt_iv[3] );
   initstate[4] = v128_32( sha256dt_iv[4] );
   initstate[5] = v128_32( sha256dt_iv[5] );
   initstate[6] = v128_32( sha256dt_iv[6] );
   initstate[7] = v128_32( sha256dt_iv[7] );

   // hash first 64 bytes of data
   sha256_4way_transform_le( midstate, vdata, initstate );

   do
   {
      sha256_4way_transform_le( block,  vdata+16, midstate  );
      sha256_4way_transform_le( hash32, block, initstate );

      mm128_block_bswap_32( hash32, hash32 );

      for ( int lane = 0; lane < 4; lane++ )
      if ( unlikely( hash32_d7[ lane ] <= targ32_d7 ) )
      {
         extr_lane_4x32( lane_hash, hash32, lane, 256 );
         if ( likely( valid_hash( lane_hash, ptarget ) && !bench ) )
         {
            pdata[19] = n + lane;
            submit_solution( work, lane_hash, mythr );
         }
      }
      *noncev = _mm_add_epi32( *noncev, four );
      n += 4;
   } while ( (n < last_nonce) && !work_restart[thr_id].restart );
   pdata[19] = n;
   *hashes_done = n - first_nonce;
   return 0;
}

#endif

bool register_sha256dt_algo( algo_gate_t* gate )
{
    gate->optimizations = SSE2_OPT | AVX2_OPT | AVX512_OPT;
#if defined(SHA256DT_16WAY)
    gate->scanhash = (void*)&scanhash_sha256dt_16way;
#elif defined(SHA256DT_SHA)
    gate->optimizations = SHA_OPT;
    gate->scanhash = (void*)&scanhash_sha256dt_sha;    
#elif defined(SHA256DT_8WAY)
    gate->scanhash = (void*)&scanhash_sha256dt_8way;
#else
    gate->scanhash = (void*)&scanhash_sha256dt_4way;
#endif
    return true;
}

