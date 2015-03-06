/* Copyright (c) 2015 Convey Computer Corporation
 *
 * This file is part of the OpenHT jpegscale application.
 *
 * Use and distribution licensed under the BSD 3-clause license.
 * See the LICENSE file for the complete license text.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define _USE_MATH_DEFINES
#include <math.h>

#include "JpegCommon.h"
#include "JpegEncodeDV.h"
#include "JpegFdctDV.h"
#include "JpegZigZag.h"
#include "JpegHuffman.h"

static inline int descale(int64_t x, int n) {
	return ( (int)x + (1 << (n-1))) >> n; 
}

void jpegEncodeRestartDV( JobInfo * pJobInfo, int rstIdx );
void jpegFdctBlkDV( JobInfo * pJobInfo, int ci, int mcuRow, int mcuCol, int blkRow, int blkCol, short (&coefBlock)[8][8] );
void jpegEncodeBlkDV( HuffEncDV & huffEnc, int16_t (&coefBlock)[8][8] );

int jpegEncBlkCntDV = 0;

void jpegEncodeDV( JobInfo * pJobInfo )
{
	// jpeg encode
	//  Perform FDCT, quantize and huffman encode

	for (int rstIdx = 0; rstIdx < pJobInfo->m_enc.m_rstCnt; rstIdx += 1)
		jpegEncodeRestartDV( pJobInfo, rstIdx );
}

#define MAX_COEF_BITS 10

void jpegEncodeRestartDV( JobInfo * pJobInfo, int rstIdx )
{
	HuffEncDV huffEnc( pJobInfo, rstIdx );

	short coefBlock[8][8];

	int mcuEncodedRowCnt = 0;

	int mcuRow = (int)(rstIdx * pJobInfo->m_enc.m_mcuRowsPerRst);
	int mcuCol = 0;

	goto restart;

	for (mcuRow = 0; mcuRow < pJobInfo->m_enc.m_mcuRows; mcuRow += 1) {
		for (mcuCol = 0; mcuCol < pJobInfo->m_enc.m_mcuCols; mcuCol += 1) {
restart:
			for (int ci = 0; ci < 3; ci += 1) {

				huffEnc.setCompId( ci );

				for (int blkRow = 0; blkRow < pJobInfo->m_enc.m_ecp[ci].m_blkRowsPerMcu; blkRow += 1) {
					for (int blkCol = 0; blkCol < pJobInfo->m_enc.m_ecp[ci].m_blkColsPerMcu; blkCol += 1) {

						jpegFdctBlkDV( pJobInfo, ci, mcuRow, mcuCol, blkRow, blkCol, coefBlock );

						jpegEncodeBlkDV( huffEnc, coefBlock );

						jpegEncBlkCntDV++;
					}
				}
			}
		}

		mcuEncodedRowCnt += 1;
		if (pJobInfo->m_enc.m_mcuRowsPerRst > 0 && pJobInfo->m_enc.m_mcuRowsPerRst == mcuEncodedRowCnt) {
			huffEnc.flushBits();
			return;
		}
	}

	// use this return if restart is not enabled
	huffEnc.flushBits();
}

void jpegFdctBlkDV( JobInfo * pJobInfo, int ci, int mcuRow, int mcuCol, int blkRow, int blkCol, int16_t (&coefBlock)[8][8] )
{
	JobEnc & enc = pJobInfo->m_enc;

	bool bLastMcuCol = mcuCol == enc.m_mcuCols-1;
	bool bLastMcuRow = mcuRow == enc.m_mcuRows-1;

	//if (bLastMcuCol && blkCol >= enc.m_ecp[ci].m_blkColsInLastMcuCol ||
	//	bLastMcuRow && blkRow >= enc.m_ecp[ci].m_blkRowsInLastMcuRow)
	//{
	//	// need to insert dummy coefBlock for huffman encoding
	//	//   coefBlock is same last generated one, just zero all AC elements
	//	memset(&coefBlock[0][1], 0, sizeof(int16_t)*63);

	//} else {
		uint8_t inBlock[8][8];

		uint64_t row = (mcuRow * enc.m_ecp[ci].m_blkRowsPerMcu + blkRow) * DCTSIZE;
		uint64_t col = (mcuCol * enc.m_ecp[ci].m_blkColsPerMcu + blkCol) * DCTSIZE;

		for (uint32_t rowIdx = 0; rowIdx < DCTSIZE; rowIdx += 1) {

			//if (row + rowIdx >= enc.m_ecp[ci].m_compRows) {
			//	// copy last image row to remaining rows
			//	*(uint64_t*)inBlock[rowIdx] = *(uint64_t*)inBlock[(enc.m_ecp[ci].m_compRows-1) & 0x7];
			//} else {
				for (uint32_t colIdx = 0; colIdx < DCTSIZE; colIdx += 1) {
					//if (col + colIdx >= enc.m_ecp[ci].m_compCols)
					//	// copy last image column pnt to remaining columns
					//	inBlock[rowIdx][colIdx] = inBlock[rowIdx][(enc.m_ecp[ci].m_compCols-1) & 0x7];
					//else {
						uint64_t imageOffset = (row + rowIdx) * enc.m_ecp[ci].m_compBufCols + col;
						//assert(imageOffset < (uint64_t)(enc.m_ecp[ci].m_compBufRows * enc.m_ecp[ci].m_compBufCols));
						inBlock[rowIdx][colIdx] = enc.m_ecp[ci].m_pCompBuf[imageOffset + colIdx];
					//}
				}
			//}
		}

#define JPEG_FDCT_DEBUG 0
#if JPEG_FDCT_DEBUG == 1
		printf("blk=%d ci=%d [%3d %3d %3d %3d %3d %3d %3d %3d]\n",
			jpegEncBlkCntDV, ci,
			inBlock[0][0], inBlock[0][1], inBlock[0][2], inBlock[0][3],
			inBlock[0][4], inBlock[0][5], inBlock[0][6], inBlock[0][7]);
		//if (jpegEncBlkCntDV == 128)
		//	exit(0);
#endif
#if JPEG_FDCT_DEBUG == 2
		if (jpegEncBlkCntDV <= 6) {
			printf("blk=%d ci=%d\n",
				jpegEncBlkCntDV, ci);
			for (int i = 0; i < 8; i += 1)
				printf("    [%3d %3d %3d %3d %3d %3d %3d %3d]\n",
				inBlock[i][0], inBlock[i][1], inBlock[i][2], inBlock[i][3],
				inBlock[i][4], inBlock[i][5], inBlock[i][6], inBlock[i][7]);
		}
		if (jpegEncBlkCntDV == 128)
			exit(0);
#endif

//static int blkCnt = 0;
//if (mcuRow==1) {
//	if (blkCnt == 37)
//		bool stop = true;
//printf("DV block(cnt=%d) mcuRow=%d\n", blkCnt++, mcuRow);
//for (int r=0; r<8; r++)
//        printf("    [%4d %4d %4d %4d %4d %4d %4d %4d]\n",
//                inBlock[r][0], inBlock[r][1],
//                inBlock[r][2], inBlock[r][3],
//                inBlock[r][4], inBlock[r][5],
//                inBlock[r][6], inBlock[r][7]);
//fflush(stdout);
//}

		jpegFdctDV ( inBlock, coefBlock );

		JobEcp & ecp = pJobInfo->m_enc.m_ecp[ci];
		int16_t (&quantTbl)[8][8] = pJobInfo->m_enc.m_dqt[ecp.m_dqtId].m_quantTbl;

		for (uint64_t col = 0; col < DCTSIZE; col += 1) {
			for (uint64_t row = 0; row < DCTSIZE; row += 1)
				coefBlock[row][col] = descale( coefBlock[row][col] * quantTbl[row][col], JPEG_ENC_RECIP_BITS + 3 );
		}
	//}
}

void jpegEncodeBlkDV( HuffEncDV & huffEnc, int16_t (&coefBlock)[8][8] )
{
	int nbits;
	int tmp, tmp2, tmp3;

#define JPEG_HUFF_ENC_DEBUG 0
#if JPEG_HUFF_ENC_DEBUG == 1
	if (jpegEncBlkCntDV < 80000)
		printf("blk=%d ci=%d [%4d %4d %4d %4d %4d %4d %4d %4d]\n",
			jpegEncBlkCntDV, huffEnc.getCompId(), coefBlock[0][0], coefBlock[0][1],
			coefBlock[0][2], coefBlock[0][3], coefBlock[0][4], coefBlock[0][5], coefBlock[0][6], coefBlock[0][7]);
#endif
#if JPEG_HUFF_ENC_DEBUG == 2
	if (jpegEncBlkCntDV <= 20000) {
		printf("blk=%d ci=%d\n",
			jpegEncBlkCntDV, huffEnc.getCompId());
		for (int i = 0; i < 8; i += 1)
			printf("    [%4d %4d %4d %4d %4d %4d %4d %4d]\n",
				coefBlock[i][0], coefBlock[i][1],
				coefBlock[i][2], coefBlock[i][3], coefBlock[i][4], coefBlock[i][5], coefBlock[i][6], coefBlock[i][7]);
	}
	if (jpegEncBlkCntDV == 20000)
		exit(0);
#endif

	/* Encode the DC coefficient difference per section F.1.2.1 */
	tmp2 = tmp = coefBlock[0][0] - huffEnc.getLastDcValueRef();
	huffEnc.getLastDcValueRef() = coefBlock[0][0];

	if (tmp < 0) {
		tmp = -tmp;
		tmp2--;
	}

	nbits = 0;
	while (tmp) {
		nbits++;
		tmp >>= 1;
	}

	/* Emit the Huffman-coded symbol for the number of bits */
	huffEnc.putNbits(nbits, true);
	huffEnc.putBits(tmp2, nbits);

	/* Encode the AC coefficients per section F.1.2.2 */
	int r = 0;			/* r = run length of zeros */
	for (uint64_t k = 1; k < DCTSIZE2; k++) {

		if ((tmp = coefBlock[jpegZigZag[k].m_r][jpegZigZag[k].m_c]) == 0) {
			r++;
		} else {
			/* if run length > 15, must emit special run-length-16 codes (0xF0) */
			while (r > 15) {
				huffEnc.putNbits(0xf0, false);
				r -= 16;
			}

			tmp2 = tmp;
			if (tmp < 0) {
				tmp = - tmp;
				tmp2--;
			}

			nbits = 1;
			while (tmp >>= 1)
				nbits++;

			tmp3 = (r << 4) + nbits; 
			huffEnc.putNbits(tmp3, false);
			huffEnc.putBits(tmp2, nbits);

			r = 0;
		}
	}

	/* If the last coef(s) were zero, emit an end-of-block code */
	if (r > 0)
		huffEnc.putNbits(0, false);
}
