/*****************************************************************************
 * macroblock.c: xavs encoder library
 *****************************************************************************
 * Copyright (C) 2009 xavs project
 *
 * Authors: 
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *****************************************************************************/

#include "common/common.h"
#include "macroblock.h"


/* 
 * xavs_mb_decimate_score: given dct coeffs it returns a score to see if we could empty this dct coeffs
 * to 0 (low score means set it to null)
 * Used in inter macroblock (luma and chroma)
 *  luma: for a 8x8 block: if score < 4 -> null
 *        for the complete mb: if score < 6 -> null
 *  chroma: for the complete mb: if score < 7 -> null
 */
static int xavs_mb_decimate_score( int *dct, int i_max )
{
    static const int i_ds_table8[64] = {
        3,3,3,3,2,2,2,2,2,2,2,2,1,1,1,1,
        1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };

    const int *ds_table = i_ds_table8;
    int i_score = 0;
    int idx = i_max - 1;

    while( idx >= 0 && dct[idx] == 0 )
        idx--;

    while( idx >= 0 )
    {
        int i_run;

        if( abs( dct[idx--] ) > 1 )
            return 9;

        i_run = 0;
        while( idx >= 0 && dct[idx] == 0 )
        {
            idx--;
            i_run++;
        }
        i_score += ds_table[i_run];
    }

    return i_score;
}

/* These chroma DC functions don't have assembly versions and are only used here. */

#define ZIG(i,y,x) level[i] = dct[x][y];
static inline void zigzag_scan_2x2_dc( int16_t level[4], int16_t dct[2][2] )
{
    ZIG(0,0,0)
    ZIG(1,0,1)
    ZIG(2,1,0)
    ZIG(3,1,1)
}
#undef ZIG

#define IDCT_DEQUANT_START \
    int d0 = dct[0][0] + dct[0][1]; \
    int d1 = dct[1][0] + dct[1][1]; \
    int d2 = dct[0][0] - dct[0][1]; \
    int d3 = dct[1][0] - dct[1][1]; \
    int dmf = dequant_mf[i_qp%6][0][0]; \
    int qbits = i_qp/6 - 5; \
    if( qbits > 0 ) \
    { \
        dmf <<= qbits; \
        qbits = 0; \
    }

static inline void idct_dequant_2x2_dc( int16_t dct[2][2], int16_t dct4x4[4][4][4], int dequant_mf[6][4][4], int i_qp )
{
    IDCT_DEQUANT_START
    dct4x4[0][0][0] = (d0 + d1) * dmf >> -qbits;
    dct4x4[1][0][0] = (d0 - d1) * dmf >> -qbits;
    dct4x4[2][0][0] = (d2 + d3) * dmf >> -qbits;
    dct4x4[3][0][0] = (d2 - d3) * dmf >> -qbits;
}

static inline void idct_dequant_2x2_dconly( int16_t dct[2][2], int dequant_mf[6][4][4], int i_qp )
{
    IDCT_DEQUANT_START
    dct[0][0] = (d0 + d1) * dmf >> -qbits;
    dct[0][1] = (d0 - d1) * dmf >> -qbits;
    dct[1][0] = (d2 + d3) * dmf >> -qbits;
    dct[1][1] = (d2 - d3) * dmf >> -qbits;
}

static inline void dct2x2dc( int16_t d[2][2], int16_t dct4x4[4][4][4] )
{
    int d0 = dct4x4[0][0][0] + dct4x4[1][0][0];
    int d1 = dct4x4[2][0][0] + dct4x4[3][0][0];
    int d2 = dct4x4[0][0][0] - dct4x4[1][0][0];
    int d3 = dct4x4[2][0][0] - dct4x4[3][0][0];
    d[0][0] = d0 + d1;
    d[1][0] = d2 + d3;
    d[0][1] = d0 - d1;
    d[1][1] = d2 - d3;
    dct4x4[0][0][0] = 0;
    dct4x4[1][0][0] = 0;
    dct4x4[2][0][0] = 0;
    dct4x4[3][0][0] = 0;
}

static inline void dct2x2dc_dconly( int16_t d[2][2] )
{
    int d0 = d[0][0] + d[0][1];
    int d1 = d[1][0] + d[1][1];
    int d2 = d[0][0] - d[0][1];
    int d3 = d[1][0] - d[1][1];
    d[0][0] = d0 + d1;
    d[1][0] = d2 + d3;
    d[0][1] = d0 - d1;
    d[1][1] = d2 - d3;
}

static ALWAYS_INLINE int xavs_quant_4x4( xavs_t *h, int16_t dct[4][4], int i_qp, int i_ctxBlockCat, int b_intra, int idx )
{
    int i_quant_cat = b_intra ? CQM_4IY : CQM_4PY;
    if( h->mb.b_trellis )
        return xavs_quant_4x4_trellis( h, dct, i_quant_cat, i_qp, i_ctxBlockCat, b_intra, 0, idx );
    else
        return h->quantf.quant_4x4( dct, h->quant4_mf[i_quant_cat][i_qp], h->quant4_bias[i_quant_cat][i_qp] );
}

static ALWAYS_INLINE int xavs_quant_8x8( xavs_t *h, int16_t dct[8][8], int i_qp, int b_intra, int idx )
{
    int i_quant_cat = b_intra ? CQM_8IY : CQM_8PY;
    if( h->mb.b_trellis )
        return xavs_quant_8x8_trellis( h, dct, i_quant_cat, i_qp, b_intra, 0, idx );
    else
        return h->quantf.quant_8x8( dct, h->quant8_mf[i_quant_cat][i_qp], h->quant8_bias[i_quant_cat][i_qp] );
}

/* All encoding functions must output the correct CBP and NNZ values.
 * The entropy coding functions will check CBP first, then NNZ, before
 * actually reading the DCT coefficients.  NNZ still must be correct even
 * if CBP is zero because of the use of NNZ values for context selection.
 * "NNZ" need only be 0 or 1 rather than the exact coefficient count because
 * that is only needed in CAVLC, and will be calculated by CAVLC's residual
 * coding and stored as necessary. */

/* This means that decimation can be done merely by adjusting the CBP and NNZ
 * rather than memsetting the coefficients. */


#define STORE_8x8_NNZ(idx,nz)\
{\
    *(uint16_t*)&h->mb.cache.non_zero_count[xavs_scan8[idx*4+0]] = nz * 0x0101;\
    *(uint16_t*)&h->mb.cache.non_zero_count[xavs_scan8[idx*4+2]] = nz * 0x0101;\
}

void xavs_mb_encode_i8x8( xavs_t *h, int idx, int i_qp )
{
    int x = 8 * (idx&1);
    int y = 8 * (idx>>1);
    int nz;
    uint8_t *p_src = &h->mb.pic.p_fenc[0][x+y*FENC_STRIDE];
    uint8_t *p_dst = &h->mb.pic.p_fdec[0][x+y*FDEC_STRIDE];
    DECLARE_ALIGNED_16( int16_t dct8x8[8][8] );

    if( h->mb.b_lossless )
    {
        nz = h->zigzagf.sub_8x8( h->dct.luma8x8[idx], p_src, p_dst );
        STORE_8x8_NNZ(idx,nz);
        h->mb.i_cbp_luma |= nz<<idx;
        return;
    }

    h->dctf.sub8x8_dct8( dct8x8, p_src, p_dst );

    nz = xavs_quant_8x8( h, dct8x8, i_qp, 1, idx );
    if( nz )
    {
        h->mb.i_cbp_luma |= 1<<idx;
        h->zigzagf.scan_8x8( h->dct.luma8x8[idx], dct8x8 );
        h->quantf.dequant_8x8( dct8x8, h->dequant8_mf[CQM_8IY], i_qp );
        h->dctf.add8x8_idct8( p_dst, dct8x8 );
        STORE_8x8_NNZ(idx,1);
    }
    else
        STORE_8x8_NNZ(idx,0);
}

static void xavs_mb_encode_i16x16( xavs_t *h, int i_qp )
{
    uint8_t  *p_src = h->mb.pic.p_fenc[0];
    uint8_t  *p_dst = h->mb.pic.p_fdec[0];

    DECLARE_ALIGNED_16( int16_t dct4x4[16][4][4] );
    DECLARE_ALIGNED_16( int16_t dct_dc4x4[4][4] );

    int i, nz;
    int b_decimate = h->sh.i_type == SLICE_TYPE_B || (h->param.analyse.b_dct_decimate && h->sh.i_type == SLICE_TYPE_P);
    int decimate_score = b_decimate ? 0 : 9;

    if( h->mb.b_lossless )
    {
        for( i = 0; i < 16; i++ )
        {
            int oe = block_idx_xy_fenc[i];
            int od = block_idx_xy_fdec[i];
            //nz = h->zigzagf.sub_4x4ac( h->dct.luma4x4[i], p_src+oe, p_dst+od, &dct_dc4x4[0][block_idx_yx_1d[i]] );
            h->mb.cache.non_zero_count[xavs_scan8[i]] = nz;
            h->mb.i_cbp_luma |= nz;
        }
        h->mb.i_cbp_luma *= 0xf;
        h->mb.cache.non_zero_count[xavs_scan8[24]] = array_non_zero( dct_dc4x4 );
        //h->zigzagf.scan_4x4( h->dct.luma16x16_dc, dct_dc4x4 );
        return;
    }

    //h->dctf.sub16x16_dct( dct4x4, p_src, p_dst );

    for( i = 0; i < 16; i++ )
    {
        /* copy dc coeff */
        dct_dc4x4[0][block_idx_xy_1d[i]] = dct4x4[i][0][0];
        dct4x4[i][0][0] = 0;

        /* quant/scan/dequant */
        nz = xavs_quant_4x4( h, dct4x4[i], i_qp, DCT_LUMA_AC, 1, i );
        h->mb.cache.non_zero_count[xavs_scan8[i]] = nz;
        if( nz )
        {
            //h->zigzagf.scan_4x4( h->dct.luma4x4[i], dct4x4[i] );
            h->quantf.dequant_4x4( dct4x4[i], h->dequant4_mf[CQM_4IY], i_qp );
            if( decimate_score < 6 ) decimate_score += h->quantf.decimate_score15( h->dct.luma4x4[i] );
            h->mb.i_cbp_luma = 0xf;
        }
    }

    /* Writing the 16 CBFs in an i16x16 block is quite costly, so decimation can save many bits. */
    /* More useful with CAVLC, but still useful with CABAC. */
    if( decimate_score < 6 )
    {
        h->mb.i_cbp_luma = 0;
        *(uint32_t*)&h->mb.cache.non_zero_count[xavs_scan8[ 0]] = 0;
        *(uint32_t*)&h->mb.cache.non_zero_count[xavs_scan8[ 2]] = 0;
        *(uint32_t*)&h->mb.cache.non_zero_count[xavs_scan8[ 8]] = 0;
        *(uint32_t*)&h->mb.cache.non_zero_count[xavs_scan8[10]] = 0;
    }

    if( h->mb.b_trellis )
        nz = xavs_quant_dc_trellis( h, (int16_t*)dct_dc4x4, CQM_4IY, i_qp, DCT_LUMA_DC, 1, 0 );
    else
        nz = h->quantf.quant_4x4_dc( dct_dc4x4, h->quant4_mf[CQM_4IY][i_qp][0]>>1, h->quant4_bias[CQM_4IY][i_qp][0]<<1 );

    h->mb.cache.non_zero_count[xavs_scan8[24]] = nz;
    if( nz )
    {
        //h->zigzagf.scan_4x4( h->dct.luma16x16_dc, dct_dc4x4 );

        /* output samples to fdec */
        //h->dctf.idct4x4dc( dct_dc4x4 );
        //h->quantf.dequant_4x4_dc( dct_dc4x4, h->dequant4_mf[CQM_4IY], i_qp );  /* XXX not inversed */
        if( h->mb.i_cbp_luma )
            for( i = 0; i < 16; i++ )
                dct4x4[i][0][0] = dct_dc4x4[0][block_idx_xy_1d[i]];
    }

    /* put pixels to fdec */
    //if( h->mb.i_cbp_luma )
       // h->dctf.add16x16_idct( p_dst, dct4x4 );
    //else if( nz )
       //h->dctf.add16x16_idct_dc( p_dst, dct_dc4x4 );
}

void xavs_mb_encode_8x8_chroma( xavs_t *h, int b_inter, int i_qp )
{
    int i, ch;
    int b_decimate = b_inter && (h->sh.i_type == SLICE_TYPE_B || h->param.analyse.b_dct_decimate);
	int nnz8x8;
	const int QP_SCALE_CR[64] =
	{
		 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
		10,11,12,13,14,15,16,17,18,19,
		20,21,22,23,24,25,26,27,28,29,
		30,31,32,33,34,35,36,37,38,39,
		40,41,42,42,43,43,44,44,45,45,
		46,46,47,47,48,48,48,49,49,49,
		50,50,50,51
	};
	int qp_chroma  = QP_SCALE_CR[i_qp]; 

    DECLARE_ALIGNED_16( int16_t dct8x8[8][8]);

    /* coded block pattern */
    h->mb.i_cbp_chroma = 0;
    for( ch = 0; ch < 2; ch++ )
    {
        uint8_t  *p_src = h->mb.pic.p_fenc[1+ch];
        uint8_t  *p_dst = h->mb.pic.p_fdec[1+ch];

		h->dctf.sub8x8_dct8( dct8x8, p_src, p_dst );

		if( h->mb.b_trellis )
                nnz8x8 = xavs_quant_8x8_trellis( h, dct8x8, CQM_8IC+b_inter, qp_chroma, !b_inter, 1, 0);
                else
                nnz8x8 = h->quantf.quant_8x8( dct8x8, h->quant8_mf[CQM_8IC+b_inter][qp_chroma], h->quant8_bias[CQM_8IC+b_inter][qp_chroma] );

		h->zigzagf.scan_8x8( h->dct.chroma8x8[ch], dct8x8 );
		
		if( b_decimate )
                nnz8x8 = 7 <= xavs_mb_decimate_score( h->dct.chroma8x8[ch], 64 );
                else
                nnz8x8 = array_non_zero( dct8x8 );

                if( nnz8x8 )
                {
                  h->quantf.dequant_8x8( dct8x8,  h->dequant8_mf[CQM_8IC + b_inter], qp_chroma );
                  h->dctf.add8x8_idct8( p_dst, dct8x8 );
                }
        
		if( nnz8x8 )
			h->mb.i_cbp_chroma |= (1 << ch);
		else
			h->mb.i_cbp_chroma &= ~(1 << ch);

		for( i = 0; i < 4; i++)
			h->mb.cache.non_zero_count[xavs_scan8[16+ch*4 + i]] = nnz8x8;
    }

}

static void xavs_macroblock_encode_skip( xavs_t *h )
{
    h->mb.i_cbp_luma = 0x00;
    h->mb.i_cbp_chroma = 0x00;
    memset( h->mb.cache.non_zero_count, 0, XAVS_SCAN8_SIZE );
    /* store cbp */
    h->mb.cbp[h->mb.i_mb_xy] = 0;
}

/*****************************************************************************
 * xavs_macroblock_encode_pskip:
 *  Encode an already marked skip block
 *****************************************************************************/
static void xavs_macroblock_encode_pskip( xavs_t *h )
{
    const int mvx = xavs_clip3( h->mb.cache.mv[0][xavs_scan8[0]][0],
                                h->mb.mv_min[0], h->mb.mv_max[0] );
    const int mvy = xavs_clip3( h->mb.cache.mv[0][xavs_scan8[0]][1],
                                h->mb.mv_min[1], h->mb.mv_max[1] );

    /* don't do pskip motion compensation if it was already done in macroblock_analyse */
    if( !h->mb.b_skip_mc )
    {
        h->mc.mc_luma( h->mb.pic.p_fdec[0],    FDEC_STRIDE,
                       h->mb.pic.p_fref[0][0], h->mb.pic.i_stride[0],
                       mvx, mvy, 16, 16 );

        h->mc.mc_chroma( h->mb.pic.p_fdec[1],       FDEC_STRIDE,
                         h->mb.pic.p_fref[0][0][4], h->mb.pic.i_stride[1],
                         mvx, mvy, 8, 8 );

        h->mc.mc_chroma( h->mb.pic.p_fdec[2],       FDEC_STRIDE,
                         h->mb.pic.p_fref[0][0][5], h->mb.pic.i_stride[2],
                         mvx, mvy, 8, 8 );
    }

    xavs_macroblock_encode_skip( h );
}

/*****************************************************************************
 * Intra prediction for predictive lossless mode.
 *****************************************************************************/

/* Note that these functions take a shortcut (mc.copy instead of actual pixel prediction) which assumes
 * that the edge pixels of the reconstructed frame are the same as that of the source frame.  This means
 * they will only work correctly if the neighboring blocks are losslessly coded.  In practice, this means
 * lossless mode cannot be mixed with lossy mode within a frame. */
/* This can be resolved by explicitly copying the edge pixels after doing the mc.copy, but this doesn't
 * need to be done unless we decide to allow mixing lossless and lossy compression. */

void xavs_predict_lossless_8x8_chroma( xavs_t *h, int i_mode )
{
    int stride = h->fenc->i_stride[1] << h->mb.b_interlaced;
    if( i_mode == I_PRED_CHROMA_V )
    {
        h->mc.copy[PIXEL_8x8]( h->mb.pic.p_fdec[1], FDEC_STRIDE, h->mb.pic.p_fenc_plane[1]-stride, stride, 8 );
        h->mc.copy[PIXEL_8x8]( h->mb.pic.p_fdec[2], FDEC_STRIDE, h->mb.pic.p_fenc_plane[2]-stride, stride, 8 );
    }
    else if( i_mode == I_PRED_CHROMA_H )
    {
        h->mc.copy[PIXEL_8x8]( h->mb.pic.p_fdec[1], FDEC_STRIDE, h->mb.pic.p_fenc_plane[1]-1, stride, 8 );
        h->mc.copy[PIXEL_8x8]( h->mb.pic.p_fdec[2], FDEC_STRIDE, h->mb.pic.p_fenc_plane[2]-1, stride, 8 );
    }
    else
    {
        h->predict_8x8c[i_mode]( h->mb.pic.p_fdec[1],h->edgeCb );
        h->predict_8x8c[i_mode]( h->mb.pic.p_fdec[2],h->edgeCr );
    }
}

void xavs_predict_lossless_4x4( xavs_t *h, uint8_t *p_dst, int idx, int i_mode )
{
    int stride = h->fenc->i_stride[0] << h->mb.b_interlaced;
    uint8_t *p_src = h->mb.pic.p_fenc_plane[0] + block_idx_x[idx]*4 + block_idx_y[idx]*4 * stride;

    if( i_mode == I_PRED_4x4_V )
        h->mc.copy[PIXEL_4x4]( p_dst, FDEC_STRIDE, p_src-stride, stride, 4 );
    else if( i_mode == I_PRED_4x4_H )
        h->mc.copy[PIXEL_4x4]( p_dst, FDEC_STRIDE, p_src-1, stride, 4 );
    else
        h->predict_4x4[i_mode]( p_dst );
}

void xavs_predict_lossless_8x8( xavs_t *h, uint8_t *p_dst, int idx, int i_mode, uint8_t edge[33] )
{
    int stride = h->fenc->i_stride[0] << h->mb.b_interlaced;
    uint8_t *p_src = h->mb.pic.p_fenc_plane[0] + (idx&1)*8 + (idx>>1)*8*stride;

    if( i_mode == I_PRED_8x8_V )
        h->mc.copy[PIXEL_8x8]( p_dst, FDEC_STRIDE, p_src-stride, stride, 8 );
    else if( i_mode == I_PRED_8x8_H )
        h->mc.copy[PIXEL_8x8]( p_dst, FDEC_STRIDE, p_src-1, stride, 8 );
    else
        h->predict_8x8[i_mode]( p_dst, edge );
}

void xavs_predict_lossless_16x16( xavs_t *h, int i_mode )
{
    int stride = h->fenc->i_stride[0] << h->mb.b_interlaced;
    if( i_mode == I_PRED_16x16_V )
        h->mc.copy[PIXEL_16x16]( h->mb.pic.p_fdec[0], FDEC_STRIDE, h->mb.pic.p_fenc_plane[0]-stride, stride, 16 );
    else if( i_mode == I_PRED_16x16_H )
        h->mc.copy_16x16_unaligned( h->mb.pic.p_fdec[0], FDEC_STRIDE, h->mb.pic.p_fenc_plane[0]-1, stride, 16 );
    else
        h->predict_16x16[i_mode]( h->mb.pic.p_fdec[0] );
}

/*****************************************************************************
 * xavs_macroblock_encode:
 *****************************************************************************/
void xavs_macroblock_encode( xavs_t *h )
{
    int i_cbp_dc = 0;
    int i_qp = h->mb.i_qp;
    int b_decimate = h->sh.i_type == SLICE_TYPE_B || h->param.analyse.b_dct_decimate;
    int b_force_no_skip = 0;
    int i,idx,nz;
    h->mb.i_cbp_luma = 0;
    h->mb.cache.non_zero_count[xavs_scan8[24]] = 0;

    if( h->sh.b_mbaff
        && h->mb.i_mb_xy == h->sh.i_first_mb + h->mb.i_mb_stride
        && IS_SKIP(h->mb.type[h->sh.i_first_mb]) )
    {
        /* The first skip is predicted to be a frame mb pair.
         * We don't yet support the aff part of mbaff, so force it to non-skip
         * so that we can pick the aff flag. */
        b_force_no_skip = 1;
        if( IS_SKIP(h->mb.i_type) )
        {
            if( h->mb.i_type == P_SKIP )
                h->mb.i_type = P_L0;
            else if( h->mb.i_type == B_SKIP )
                h->mb.i_type = B_DIRECT;
        }
    }

    if( h->mb.i_type == P_SKIP )
    {
        /* A bit special */
        xavs_macroblock_encode_pskip( h );
        return;
    }
    if( h->mb.i_type == B_SKIP )
    {
        /* don't do bskip motion compensation if it was already done in macroblock_analyse */
        if( !h->mb.b_skip_mc )
            xavs_mb_mc( h );
        xavs_macroblock_encode_skip( h );
        return;
    }

    if( h->mb.i_type == I_16x16 )
    {
        const int i_mode = h->mb.i_intra16x16_pred_mode;
        h->mb.b_transform_8x8 = 0;

        if( h->mb.b_lossless )
            xavs_predict_lossless_16x16( h, i_mode );
        else
            h->predict_16x16[i_mode]( h->mb.pic.p_fdec[0] );

        /* encode the 16x16 macroblock */
        xavs_mb_encode_i16x16( h, i_qp );
    }
    else if( h->mb.i_type == I_8x8 )
    {
        DECLARE_ALIGNED_16( uint8_t edge[33] );
        h->mb.b_transform_8x8 = 1;
        /* If we already encoded 3 of the 4 i8x8 blocks, we don't have to do them again. */
        if( h->mb.i_skip_intra )
        {
			//aloha'
            //h->mc.copy[PIXEL_16x16]( h->mb.pic.p_fdec[0], FDEC_STRIDE, h->mb.pic.i8x8_fdec_buf, 16, 16 );
			h->mc.copy[PIXEL_16x16]( h->mb.pic.i8x8_fdec_buf, 16, h->mb.pic.p_fdec[0], FDEC_STRIDE, 16 );
            *(uint32_t*)&h->mb.cache.non_zero_count[xavs_scan8[ 0]] = h->mb.pic.i8x8_nnz_buf[0];
            *(uint32_t*)&h->mb.cache.non_zero_count[xavs_scan8[ 2]] = h->mb.pic.i8x8_nnz_buf[1];
            *(uint32_t*)&h->mb.cache.non_zero_count[xavs_scan8[ 8]] = h->mb.pic.i8x8_nnz_buf[2];
            *(uint32_t*)&h->mb.cache.non_zero_count[xavs_scan8[10]] = h->mb.pic.i8x8_nnz_buf[3];

			//aloha'
			h->mb.i_cbp_luma = h->mb.i_skip_intra ? (h->mb.pic.i8x8_cbp & 7) : 0;
			//h->mb.i_cbp_luma = h->mb.i_skip_intra;

            /* In RD mode, restore the now-overwritten DCT data. */
            if( h->mb.i_skip_intra == 2 )
                h->mc.memcpy_aligned( h->dct.luma8x8, h->mb.pic.i8x8_dct_buf, sizeof(h->mb.pic.i8x8_dct_buf) );
        }

        for( i = h->mb.i_skip_intra ? 3 : 0 ; i < 4; i++ )
        {
            uint8_t  *p_dst = &h->mb.pic.p_fdec[0][8 * (i&1) + 8 * (i>>1) * FDEC_STRIDE];
            int      i_mode = h->mb.cache.intra4x4_pred_mode[xavs_scan8[4*i]];
            h->predict_8x8_filter( p_dst, edge, h->mb.i_neighbour8[i], xavs_pred_i4x4_neighbors[i_mode] );

            if( h->mb.b_lossless )
                xavs_predict_lossless_8x8( h, p_dst, i, i_mode, edge );
            else
                h->predict_8x8[i_mode]( p_dst, edge );

            xavs_mb_encode_i8x8( h, i, i_qp );
        }
    }
   else    /* Inter MB */
    {
        int i8x8, i4x4;
        int i_decimate_mb = 0;

        /* Don't repeat motion compensation if it was already done in non-RD transform analysis */
        if( !h->mb.b_skip_mc )
            xavs_mb_mc( h );

        if( h->mb.b_lossless )
        {
            if( h->mb.b_transform_8x8 )
                for( i8x8 = 0; i8x8 < 4; i8x8++ )
                {
                    int x = 8*(i8x8&1);
                    int y = 8*(i8x8>>1);
                    nz = h->zigzagf.sub_8x8( h->dct.luma8x8[i8x8],
                                        h->mb.pic.p_fenc[0]+x+y*FENC_STRIDE,
                                        h->mb.pic.p_fdec[0]+x+y*FDEC_STRIDE );
                    STORE_8x8_NNZ(i8x8,nz);
                    h->mb.i_cbp_luma |= nz << i8x8;
                }
            else
                for( i4x4 = 0; i4x4 < 16; i4x4++ )
                {
//                    nz = h->zigzagf.sub_4x4( h->dct.luma4x4[i4x4],
                                        //h->mb.pic.p_fenc[0]+block_idx_xy_fenc[i4x4],
                                        //h->mb.pic.p_fdec[0]+block_idx_xy_fdec[i4x4] );
                    h->mb.cache.non_zero_count[xavs_scan8[i4x4]] = nz;
                    h->mb.i_cbp_luma |= nz << (i4x4>>2);
                }
        }
        else if( h->mb.b_transform_8x8 )
        {
            DECLARE_ALIGNED_16( int16_t dct8x8[4][8][8] );
            b_decimate &= !h->mb.b_trellis; // 8x8 trellis is inherently optimal decimation
            h->dctf.sub16x16_dct8( dct8x8, h->mb.pic.p_fenc[0], h->mb.pic.p_fdec[0] );
            h->nr_count[1] += h->mb.b_noise_reduction * 4;

            for( idx = 0; idx < 4; idx++ )
            {
                if( h->mb.b_noise_reduction )
                    h->quantf.denoise_dct( *dct8x8[idx], h->nr_residual_sum[1], h->nr_offset[1], 64 );
                nz = xavs_quant_8x8( h, dct8x8[idx], i_qp, 0, idx );

                if( nz )
                {
                    h->zigzagf.scan_8x8( h->dct.luma8x8[idx], dct8x8[idx] );
                    if( b_decimate )
                    {
                        int i_decimate_8x8 = h->quantf.decimate_score64( h->dct.luma8x8[idx] );
                        i_decimate_mb += i_decimate_8x8;
                        if( i_decimate_8x8 >= 4 )
                            h->mb.i_cbp_luma |= 1<<idx;
                    }
                    else
                        h->mb.i_cbp_luma |= 1<<idx;
                }
            }

            if( i_decimate_mb < 6 && b_decimate )
            {
                h->mb.i_cbp_luma = 0;
                *(uint32_t*)&h->mb.cache.non_zero_count[xavs_scan8[ 0]] = 0;
                *(uint32_t*)&h->mb.cache.non_zero_count[xavs_scan8[ 2]] = 0;
                *(uint32_t*)&h->mb.cache.non_zero_count[xavs_scan8[ 8]] = 0;
                *(uint32_t*)&h->mb.cache.non_zero_count[xavs_scan8[10]] = 0;
            }
            else
            {
                for( idx = 0; idx < 4; idx++ )
                {
                    if( h->mb.i_cbp_luma&(1<<idx) )
                    {
                        h->quantf.dequant_8x8( dct8x8[idx], h->dequant8_mf[CQM_8PY], i_qp );
                        h->dctf.add8x8_idct8( &h->mb.pic.p_fdec[0][(idx&1)*8 + (idx>>1)*8*FDEC_STRIDE], dct8x8[idx] );
                        STORE_8x8_NNZ(idx,1);
                    }
                    else
                        STORE_8x8_NNZ(idx,0);
                }
            }
        }
     }

    /* encode chroma */
    if( IS_INTRA( h->mb.i_type ) )
    {
        const int i_mode = h->mb.i_chroma_pred_mode;
        if( h->mb.b_lossless )
            xavs_predict_lossless_8x8_chroma( h, i_mode );
        else
        {
            h->predict_8x8c[i_mode]( h->mb.pic.p_fdec[1],h->edgeCb);
            h->predict_8x8c[i_mode]( h->mb.pic.p_fdec[2],h->edgeCr );
        }
    }

    /* encode the 8x8 blocks */
    xavs_mb_encode_8x8_chroma( h, !IS_INTRA( h->mb.i_type ), h->mb.i_chroma_qp );

    if( h->param.b_cabac )
    {
        i_cbp_dc = h->mb.cache.non_zero_count[xavs_scan8[24]]
                 | h->mb.cache.non_zero_count[xavs_scan8[25]] << 1
                 | h->mb.cache.non_zero_count[xavs_scan8[26]] << 2;
    }

    /* store cbp */
    h->mb.cbp[h->mb.i_mb_xy] = (i_cbp_dc << 8) | (h->mb.i_cbp_chroma << 4) | h->mb.i_cbp_luma;

    /* Check for P_SKIP
     * XXX: in the me perhaps we should take xavs_mb_predict_mv_pskip into account
     *      (if multiple mv give same result)*/
    if( !b_force_no_skip )
    {
        if( h->mb.i_type == P_L0 && h->mb.i_partition == D_16x16 &&
            !(h->mb.i_cbp_luma | h->mb.i_cbp_chroma) &&
            *(uint32_t*)h->mb.cache.mv[0][xavs_scan8[0]] == *(uint32_t*)h->mb.cache.pskip_mv
            && h->mb.cache.ref[0][xavs_scan8[0]] == 0 )
        {
            h->mb.i_type = P_SKIP;
        }

        /* Check for B_SKIP */
        if( h->mb.i_type == B_DIRECT && !(h->mb.i_cbp_luma | h->mb.i_cbp_chroma) )
        {
            h->mb.i_type = B_SKIP;
        }
    }
}

/*****************************************************************************
 * xavs_macroblock_probe_skip:
 *  Check if the current MB could be encoded as a [PB]_SKIP (it supposes you use
 *  the previous QP
 *****************************************************************************/
int xavs_macroblock_probe_skip( xavs_t *h, int b_bidir )
{
    DECLARE_ALIGNED_16( int16_t dct4x4[4][4][4] );
    DECLARE_ALIGNED_16( int16_t dct2x2[2][2] );
    DECLARE_ALIGNED_16( int16_t dctscan[16] );

    int i_qp = h->mb.i_qp;
    int mvp[2];
    int ch, thresh, ssd;

    int i8x8, i4x4;
    int i_decimate_mb;

    if( !b_bidir )
    {
        /* Get the MV */
        mvp[0] = xavs_clip3( h->mb.cache.pskip_mv[0], h->mb.mv_min[0], h->mb.mv_max[0] );
        mvp[1] = xavs_clip3( h->mb.cache.pskip_mv[1], h->mb.mv_min[1], h->mb.mv_max[1] );

        /* Motion compensation */
        h->mc.mc_luma( h->mb.pic.p_fdec[0],    FDEC_STRIDE,
                       h->mb.pic.p_fref[0][0], h->mb.pic.i_stride[0],
                       mvp[0], mvp[1], 16, 16 );
    }

    for( i8x8 = 0, i_decimate_mb = 0; i8x8 < 4; i8x8++ )
    {
        int fenc_offset = (i8x8&1) * 8 + (i8x8>>1) * FENC_STRIDE * 8;
        int fdec_offset = (i8x8&1) * 8 + (i8x8>>1) * FDEC_STRIDE * 8;
        /* get luma diff */
//        h->dctf.sub8x8_dct( dct4x4, h->mb.pic.p_fenc[0] + fenc_offset,
//                                    h->mb.pic.p_fdec[0] + fdec_offset );
        /* encode one 4x4 block */
        for( i4x4 = 0; i4x4 < 4; i4x4++ )
        {
            if( !h->quantf.quant_4x4( dct4x4[i4x4], h->quant4_mf[CQM_4PY][i_qp], h->quant4_bias[CQM_4PY][i_qp] ) )
                continue;
            //h->zigzagf.scan_4x4( dctscan, dct4x4[i4x4] );
            i_decimate_mb += h->quantf.decimate_score16( dctscan );
            if( i_decimate_mb >= 6 )
                return 0;
        }
    }

    /* encode chroma */
    i_qp = h->mb.i_chroma_qp;
    thresh = (xavs_lambda2_tab[i_qp] + 32) >> 6;

    for( ch = 0; ch < 2; ch++ )
    {
        uint8_t  *p_src = h->mb.pic.p_fenc[1+ch];
        uint8_t  *p_dst = h->mb.pic.p_fdec[1+ch];

        if( !b_bidir )
        {
            h->mc.mc_chroma( h->mb.pic.p_fdec[1+ch],       FDEC_STRIDE,
                             h->mb.pic.p_fref[0][0][4+ch], h->mb.pic.i_stride[1+ch],
                             mvp[0], mvp[1], 8, 8 );
        }

        /* there is almost never a termination during chroma, but we can't avoid the check entirely */
        /* so instead we check SSD and skip the actual check if the score is low enough. */
        ssd = h->pixf.ssd[PIXEL_8x8]( p_dst, FDEC_STRIDE, p_src, FENC_STRIDE );
        if( ssd < thresh )
            continue;

//        h->dctf.sub8x8_dct( dct4x4, p_src, p_dst );

        /* calculate dct DC */
        dct2x2dc( dct2x2, dct4x4 );
        if( h->quantf.quant_2x2_dc( dct2x2, h->quant4_mf[CQM_4PC][i_qp][0]>>1, h->quant4_bias[CQM_4PC][i_qp][0]<<1 ) )
            return 0;

        /* If there wasn't a termination in DC, we can check against a much higher threshold. */
        if( ssd < thresh*4 )
            continue;

        /* calculate dct coeffs */
        for( i4x4 = 0, i_decimate_mb = 0; i4x4 < 4; i4x4++ )
        {
            if( !h->quantf.quant_4x4( dct4x4[i4x4], h->quant4_mf[CQM_4PC][i_qp], h->quant4_bias[CQM_4PC][i_qp] ) )
                continue;
            //h->zigzagf.scan_4x4( dctscan, dct4x4[i4x4] );
            i_decimate_mb += h->quantf.decimate_score15( dctscan );
            if( i_decimate_mb >= 7 )
                return 0;
        }
    }

    h->mb.b_skip_mc = 1;
    return 1;
}

/****************************************************************************
 * DCT-domain noise reduction / adaptive deadzone
 * from libavcodec
 ****************************************************************************/

void xavs_noise_reduction_update( xavs_t *h )
{

}

/*****************************************************************************
 * RD only; 4 calls to this do not make up for one macroblock_encode.
 * doesn't transform chroma dc.
 *****************************************************************************/
void xavs_macroblock_encode_p8x8( xavs_t *h, int i8 )
{
    int i_qp = h->mb.i_qp;
    uint8_t *p_fenc = h->mb.pic.p_fenc[0] + (i8&1)*8 + (i8>>1)*8*FENC_STRIDE;
    uint8_t *p_fdec = h->mb.pic.p_fdec[0] + (i8&1)*8 + (i8>>1)*8*FDEC_STRIDE;
    int b_decimate = h->sh.i_type == SLICE_TYPE_B || h->param.analyse.b_dct_decimate;
    int nnz8x8 = 0;
    int ch, nz;

    if( !h->mb.b_skip_mc )
        xavs_mb_mc_8x8( h, i8 );

    if( h->mb.b_lossless )
    {
        int i4;
        if( h->mb.b_transform_8x8 )
        {
            nnz8x8 = h->zigzagf.sub_8x8( h->dct.luma8x8[i8], p_fenc, p_fdec );
            STORE_8x8_NNZ(i8,nnz8x8);
        }
        else
        {
            for( i4 = i8*4; i4 < i8*4+4; i4++ )
            {
                int nz;
//                nz = h->zigzagf.sub_4x4( h->dct.luma4x4[i4],
//                                    h->mb.pic.p_fenc[0]+block_idx_xy_fenc[i4],
//                                    h->mb.pic.p_fdec[0]+block_idx_xy_fdec[i4] );
                h->mb.cache.non_zero_count[xavs_scan8[i4]] = nz;
                nnz8x8 |= nz;
            }
        }
        for( ch = 0; ch < 2; ch++ )
        {
            int16_t dc;
            p_fenc = h->mb.pic.p_fenc[1+ch] + (i8&1)*4 + (i8>>1)*4*FENC_STRIDE;
            p_fdec = h->mb.pic.p_fdec[1+ch] + (i8&1)*4 + (i8>>1)*4*FDEC_STRIDE;
            //nz = h->zigzagf.sub_4x4ac( h->dct.luma4x4[16+i8+ch*4], p_fenc, p_fdec, &dc );
            h->mb.cache.non_zero_count[xavs_scan8[16+i8+ch*4]] = nz;
        }
    }
    else
    {
        if( h->mb.b_transform_8x8 )
        {
            DECLARE_ALIGNED_16( int16_t dct8x8[8][8] );
            h->dctf.sub8x8_dct8( dct8x8, p_fenc, p_fdec );
            nnz8x8 = xavs_quant_8x8( h, dct8x8, i_qp, 0, i8 );
            if( nnz8x8 )
            {
                h->zigzagf.scan_8x8( h->dct.luma8x8[i8], dct8x8 );

                if( b_decimate && !h->mb.b_trellis )
                    nnz8x8 = 4 <= h->quantf.decimate_score64( h->dct.luma8x8[i8] );

                if( nnz8x8 )
                {
                    h->quantf.dequant_8x8( dct8x8, h->dequant8_mf[CQM_8PY], i_qp );
                    h->dctf.add8x8_idct8( p_fdec, dct8x8 );
                    STORE_8x8_NNZ(i8,1);
                }
                else
                    STORE_8x8_NNZ(i8,0);
            }
            else
                STORE_8x8_NNZ(i8,0);
        }
       
        i_qp = h->mb.i_chroma_qp;

        for( ch = 0; ch < 2; ch++ )
        {
            DECLARE_ALIGNED_16( int16_t dct4x4[4][4] );
            p_fenc = h->mb.pic.p_fenc[1+ch] + (i8&1)*4 + (i8>>1)*4*FENC_STRIDE;
            p_fdec = h->mb.pic.p_fdec[1+ch] + (i8&1)*4 + (i8>>1)*4*FDEC_STRIDE;

//            h->dctf.sub4x4_dct( dct4x4, p_fenc, p_fdec );
            dct4x4[0][0] = 0;

            if( h->mb.b_trellis )
                nz = xavs_quant_4x4_trellis( h, dct4x4, CQM_4PC, i_qp, DCT_CHROMA_AC, 0, 1, 0 );
            else
                nz = h->quantf.quant_4x4( dct4x4, h->quant4_mf[CQM_4PC][i_qp], h->quant4_bias[CQM_4PC][i_qp] );

            h->mb.cache.non_zero_count[xavs_scan8[16+i8+ch*4]] = nz;
            if( nz )
            {
                //h->zigzagf.scan_4x4( h->dct.luma4x4[16+i8+ch*4], dct4x4 );
                //h->quantf.dequant_4x4( dct4x4, h->dequant4_mf[CQM_4PC], i_qp );
                //h->dctf.add4x4_idct( p_fdec, dct4x4 );
            }
        }
    }
    h->mb.i_cbp_luma &= ~(1 << i8);
    h->mb.i_cbp_luma |= nnz8x8 << i8;
    h->mb.i_cbp_chroma = 0x02;
}

/*****************************************************************************
 * RD only, luma only
 *****************************************************************************/
void xavs_macroblock_encode_p4x4( xavs_t *h, int i4 )
{
   
}