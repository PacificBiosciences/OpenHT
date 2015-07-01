#include "Ht.h"
#include "PersWr3.h"

void CPersWr3::PersWr3()
{
	if (PR_htValid) {
		switch (PR_htInst) {
		case WR3_INIT:
		{
			P_loopCnt = 0;
			P_err = 0;
			P_pauseLoopCnt = 0;

			HtContinue(WR3_WRITE1b);
		}
		break;
		case WR3_WRITE1a:
		case WR3_WRITE1b:
		{
			if (PR_htInst != (PR_pauseDst ? WR3_WRITE1a : WR3_WRITE1b)) {
				HtAssert(0, 0);
				P_err += 1;
			}
			P_pauseDst ^= 1;

			if (WriteMemBusy() || SendReturnBusy_wr3()) {
				HtRetry();
				break;
			}

			// Check if end of loop
			if (P_loopCnt == PAUSE_LOOP_CNT || P_err) {
				// Return to host interface
				SendReturn_wr3(P_err);
			} else {
				// Calculate memory read address
				MemAddr_t memRdAddr = P_arrayAddr + ((P_loopCnt & 0xf) << 3);

				// Issue read request to memory
				WriteMem(memRdAddr, (P_loopCnt & 0xf));

				// Set address for reading memory response data

				HtContinue(WR3_WRITE2);
			}
		}
		break;
		case WR3_WRITE2:
		{
			if (WriteMemBusy()) {
				HtRetry();
				break;
			}

			// Calculate memory read address
			MemAddr_t memRdAddr = P_arrayAddr + (((P_loopCnt + 1) & 0xf) << 3);

			// Issue read request to memory
			WriteMem(memRdAddr, ((P_loopCnt + 1) & 0xf));

			HtContinue(WR3_WRITE3);
		}
		break;
		case WR3_WRITE3:
		{
			if (WriteMemBusy()) {
				HtRetry();
				break;
			}

			// Calculate memory read address
			MemAddr_t memRdAddr = P_arrayAddr + (((P_loopCnt + 2) & 0xf) << 3);

			// Issue read request to memory
			WriteMem(memRdAddr, ((P_loopCnt + 2) & 0xf));

			HtContinue(WR3_WRITE4);
		}
		break;
		case WR3_WRITE4:
		{
			if (WriteMemBusy()) {
				HtRetry();
				break;
			}

			// Calculate memory read address
			MemAddr_t memRdAddr = P_arrayAddr + (((P_loopCnt + 3) & 0xf) << 3);

			// Issue read request to memory
			WriteMem(memRdAddr, ((P_loopCnt + 3) & 0xf));

			HtContinue(WR3_LOOP);
		}
		break;
		case WR3_LOOP:
		{
			if (WriteMemBusy()) {
				HtRetry();
				break;
			}

			// wait a few instructions for last response to line up with call to ReadMemPause
			if (P_pauseLoopCnt == 2) {
				P_pauseLoopCnt = 0;
				P_loopCnt += 1;

				if (PR_pauseDst)
					WriteMemPause(WR3_WRITE1a);
				else
					WriteMemPause(WR3_WRITE1b);
			} else {
				P_pauseLoopCnt += 1;
				HtContinue(WR3_LOOP);
			}
		}
		break;
		default:
			assert(0);
		}
	}

#ifndef _HTV
	if (r_m1_wrRspRdy) {
#if (WR3_HTID_W == 0)
		if (!r_wrGrpRsmWait)
			printf("-");
		else if (r_wrGrpRspCnt == 1)
			printf("1");
		else
			printf("+");
#else
#if (WR3_HTID_W <= 2)
		if (r_wrGrpState[r_m1_wrGrpId].m_pause)
			printf("-");
		else if (r_wrGrpState[r_m1_wrGrpId].m_cnt == 1)
			printf("3");
		else
			printf("+");
#else
		m_rdGrpRsmWait[r_m1_rdRspGrpId & 3].read_addr(r_m1_rdRspGrpId >> 2);
		if (!m_rdGrpRsmWait[r_m1_rdRspGrpId & 3].read_mem())
			printf("-");
		else if (r_rdGrpRspCnt[r_m1_rdRspGrpId & 3] == 1)
			printf("1");
		else
			printf("+");
#endif
#endif
	}
#endif
}
