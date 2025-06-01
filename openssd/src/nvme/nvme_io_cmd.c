//////////////////////////////////////////////////////////////////////////////////
// nvme_io_cmd.c for Cosmos+ OpenSSD
// Copyright (c) 2016 Hanyang University ENC Lab.
// Contributed by Yong Ho Song <yhsong@enc.hanyang.ac.kr>
//				  Youngjin Jo <yjjo@enc.hanyang.ac.kr>
//				  Sangjin Lee <sjlee@enc.hanyang.ac.kr>
//				  Jaewook Kwak <jwkwak@enc.hanyang.ac.kr>
//
// This file is part of Cosmos+ OpenSSD.
//
// Cosmos+ OpenSSD is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// Cosmos+ OpenSSD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Cosmos+ OpenSSD; see the file COPYING.
// If not, see <http://www.gnu.org/licenses/>.
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Company: ENC Lab. <http://enc.hanyang.ac.kr>
// Engineer: Sangjin Lee <sjlee@enc.hanyang.ac.kr>
//			 Jaewook Kwak <jwkwak@enc.hanyang.ac.kr>
//
// Project Name: Cosmos+ OpenSSD
// Design Name: Cosmos+ Firmware
// Module Name: NVMe IO Command Handler
// File Name: nvme_io_cmd.c
//
// Version: v1.0.1
//
// Description:
//   - handles NVMe IO command
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Revision History:
//
// * v1.0.1
//   - header file for buffer is changed from "ia_lru_buffer.h" to "lru_buffer.h"
//
// * v1.0.0
//   - First draft
//////////////////////////////////////////////////////////////////////////////////


#include "xil_printf.h"
#include "debug.h"
#include "io_access.h"

#include "nvme.h"
#include "host_lld.h"
#include "nvme_io_cmd.h"
#include "cxl_ssd.h"
#include "../memory_map.h"

#include "../ftl_config.h"
#include "../request_transform.h"

static void TriggerInternalDataWrite(const unsigned int lsa, const unsigned int bufAddr) {
    unsigned int virtualSliceAddr = AddrTransWrite(lsa);
    unsigned int reqSlotTag = GetFromFreeReqQ();

    reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
    reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_WRITE;
    reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = lsa;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
    reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.addr = bufAddr;
    reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = virtualSliceAddr;

    SelectLowLevelReqQ(reqSlotTag);
}

void handle_nvme_io_read(unsigned int cmdSlotTag, NVME_IO_COMMAND *nvmeIOCmd)
{
	IO_READ_COMMAND_DW12 readInfo12;
	//IO_READ_COMMAND_DW13 readInfo13;
	//IO_READ_COMMAND_DW15 readInfo15;
	unsigned int startLba[2];
	unsigned int nlb;

	readInfo12.dword = nvmeIOCmd->dword[12];
	//readInfo13.dword = nvmeIOCmd->dword[13];
	//readInfo15.dword = nvmeIOCmd->dword[15];

	startLba[0] = nvmeIOCmd->dword[10];
	startLba[1] = nvmeIOCmd->dword[11];
	nlb = readInfo12.NLB;

	ASSERT(startLba[0] < storageCapacity_L && (startLba[1] < STORAGE_CAPACITY_H || startLba[1] == 0));
	//ASSERT(nlb < MAX_NUM_OF_NLB);
	ASSERT((nvmeIOCmd->PRP1[0] & 0x3) == 0 && (nvmeIOCmd->PRP2[0] & 0x3) == 0); //error
	ASSERT(nvmeIOCmd->PRP1[1] < 0x10000 && nvmeIOCmd->PRP2[1] < 0x10000);

	ReqTransNvmeToSlice(cmdSlotTag, startLba[0], nlb, IO_NVM_READ);
}


void handle_nvme_io_write(unsigned int cmdSlotTag, NVME_IO_COMMAND *nvmeIOCmd)
{
	IO_READ_COMMAND_DW12 writeInfo12;
	//IO_READ_COMMAND_DW13 writeInfo13;
	//IO_READ_COMMAND_DW15 writeInfo15;
	unsigned int startLba[2];
	unsigned int nlb;

	writeInfo12.dword = nvmeIOCmd->dword[12];
	//writeInfo13.dword = nvmeIOCmd->dword[13];
	//writeInfo15.dword = nvmeIOCmd->dword[15];

	//if(writeInfo12.FUA == 1)
	//	xil_printf("write FUA\r\n");

	startLba[0] = nvmeIOCmd->dword[10];
	startLba[1] = nvmeIOCmd->dword[11];
	nlb = writeInfo12.NLB;

	ASSERT(startLba[0] < storageCapacity_L && (startLba[1] < STORAGE_CAPACITY_H || startLba[1] == 0));
	//ASSERT(nlb < MAX_NUM_OF_NLB);
	ASSERT((nvmeIOCmd->PRP1[0] & 0xF) == 0 && (nvmeIOCmd->PRP2[0] & 0xF) == 0);
	ASSERT(nvmeIOCmd->PRP1[1] < 0x10000 && nvmeIOCmd->PRP2[1] < 0x10000);

	ReqTransNvmeToSlice(cmdSlotTag, startLba[0], nlb, IO_NVM_WRITE);
}

void handle_nvme_single_trace(unsigned int cmdSlotTag, NVME_IO_COMMAND *nvmeIOCmd) {
    NVME_COMPLETION nvmeCPL;

    unsigned int addrStart = nvmeIOCmd->dword[10];
    unsigned int addrEnd = nvmeIOCmd->dword[11];
    unsigned int cmdType = nvmeIOCmd->dword[12];
    uint64_t time_counter, tmiss_counter = 0;
    unsigned short status;

    //xil_printf("CXL request: %d \r\n", cmdType);
    if(cmdType == 0){ //memory fetch
    	status = MemReadRequestSingle(addrStart, addrEnd, &time_counter, &tmiss_counter);
    } else { //memory store
    	status = MemWriteRequestSingle(addrStart, addrEnd, nvmeIOCmd, &time_counter);
    }
    //xil_printf("CXL request done in: %d \r\n", (uint32_t)time_counter);
    nvmeCPL.dword[0] = 0;
    nvmeCPL.specific = time_counter;
    nvmeCPL.statusField.SCT = 0x0;
    //xil_printf("%llu", tmiss_counter);
    if (tmiss_counter == 0)
    	nvmeCPL.statusField.SC = status;
    else
    	nvmeCPL.statusField.SC = (unsigned short)tmiss_counter;
    set_auto_nvme_cpl(cmdSlotTag, nvmeCPL.specific, nvmeCPL.statusFieldWord);
}

void handle_nvme_write_page(unsigned int cmdSlotTag, NVME_IO_COMMAND *nvmeIOCmd) {
    NVME_COMPLETION nvmeCPL;

    unsigned int addrStart = nvmeIOCmd->dword[10];
    unsigned int addrEnd = nvmeIOCmd->dword[11];
    unsigned int cmdType = nvmeIOCmd->dword[12];

    //xil_printf("Write page start: %d \r\n", addrEnd);
    TriggerInternalDataWrite(addrEnd / BYTES_PER_DATA_REGION_OF_SLICE, nvmeIOCmd);
    SyncAllLowLevelReqDone();
    //xil_printf("Write page end: %d \r\n", addrEnd);

    nvmeCPL.dword[0] = 0;
    nvmeCPL.specific = 0x0;
    set_auto_nvme_cpl(cmdSlotTag, nvmeCPL.specific, nvmeCPL.statusFieldWord);
}

void handle_stats(unsigned int cmdSlotTag, NVME_IO_COMMAND *nvmeIOCmd) {
    NVME_COMPLETION nvmeCPL;

    xil_printf("==== SSD Statistics ====\r\n");

	xil_printf("block_rissue_count %llu\r\n", stats.block_rissue_count);
	xil_printf("block_wissue_count %llu\r\n", stats.block_wissue_count);
	xil_printf("byte_rissue_count %llu\r\n", stats.byte_rissue_count);
	xil_printf("byte_wissue_count %llu\r\n", stats.byte_wissue_count);

	xil_printf("block_rissue_traffic %llu\r\n", stats.block_rissue_traffic);
	xil_printf("block_wissue_traffic %llu\r\n", stats.block_wissue_traffic);
	xil_printf("byte_rissue_traffic %llu\r\n", stats.byte_rissue_traffic);
	xil_printf("byte_wissue_traffic %llu\r\n", stats.byte_wissue_traffic);

	xil_printf("log_compaction_count %llu\r\n", stats.log_compaction_count);
	xil_printf("log_compaction_traffic_read %llu\r\n", stats.log_compaction_traffic_read);
	xil_printf("log_compaction_traffic_write %llu\r\n", stats.log_compaction_traffic_write);

	xil_printf("nand_read_user %llu\r\n", stats.nand_read_user);
	xil_printf("nand_read_internal %llu\r\n", stats.nand_read_internal);
	xil_printf("nand_write_user %llu\r\n", stats.nand_write_user);
	xil_printf("nand_write_internal %llu\r\n", stats.nand_write_internal);

    nvmeCPL.dword[0] = 0;
    nvmeCPL.specific = 0x0;
    set_auto_nvme_cpl(cmdSlotTag, nvmeCPL.specific, nvmeCPL.statusFieldWord);
}

/*
void handle_nvme_io_trace_push(unsigned int cmdSlotTag, NVME_IO_COMMAND *nvmeIOCmd) {
    NVME_COMPLETION nvmeCPL;
    unsigned int data_len, request_count, trace_size;
    unsigned int numOfNvmeBlock, curNumOfNvmeBlock, dmaIndex;
    unsigned int trace_addr = TRACE_DMA_BUFFER_START;
    unsigned int trace_op, trace_addr_val, trace_payload[CACHELINE_SIZE / sizeof(unsigned int)];

    request_count = nvmeIOCmd->dword[10];  // req count
    data_len = nvmeIOCmd->dword[13];  // trace batch size
    numOfNvmeBlock = data_len / BYTES_PER_NVME_BLOCK;

    // trigger rx dma for memory trace batch at the host-side
    dmaIndex = curNumOfNvmeBlock = 0;
    while (curNumOfNvmeBlock <= numOfNvmeBlock) {
        set_auto_rx_dma(cmdSlotTag, dmaIndex, trace_addr + (dmaIndex * BYTES_PER_NVME_BLOCK), NVME_COMMAND_AUTO_COMPLETION_OFF);
        curNumOfNvmeBlock++;
        dmaIndex++;
    }
    check_auto_rx_dma_done();

    // parse memory trace data to perform cxl.mem read or write
    for (unsigned int *p = (unsigned int *)trace_addr; p < (unsigned int *)(trace_addr + data_len);) {
        trace_op = *p;                                 // opcode: read(0) / write(1)
        trace_addr_val = *(p + 1);                     // memory address
        trace_size = *(p + 2);                         // size (always 64 in this case)

        // cxl.mem read request
        if (trace_op == 0) {
            xil_printf("Processing Read: Addr 0x%lx\r\n", (uint32_t)trace_addr_val);
            MemReadRequest((uint32_t)trace_addr_val, (uint8_t *)trace_payload);
            p += 3;   // move to next trace entry (opcode + addr + size = 12B)
        }
        // cxl.mem write request
        else if (trace_op == 1) {
            xil_printf("Processing Write: Addr 0x%lx\r\n", (uint32_t)trace_addr_val);
            memcpy(trace_payload, p + 3, CACHELINE_SIZE);  // payload(64B)
            MemWriteRequest((uint32_t)trace_addr_val, (uint8_t *)trace_payload);
            p += (3 + CACHELINE_SIZE / sizeof(unsigned int));  // move to next trace entry (76B)
        }
        // error
        else {
            xil_printf("[ERROR] Invalid Trace Operation %u at Addr 0x%lx\r\n", trace_op, (uint32_t)trace_addr_val);
            assert(0);
        }
        xil_printf("////////////////////////////////////////////////////////\r\n");
    }
    nvmeCPL.dword[0] = 0;
    nvmeCPL.specific = 0x0;
    set_auto_nvme_cpl(cmdSlotTag, nvmeCPL.specific, nvmeCPL.statusFieldWord);
}
*/


void handle_nvme_io_cmd(NVME_COMMAND *nvmeCmd)
{
	NVME_IO_COMMAND *nvmeIOCmd;
	NVME_COMPLETION nvmeCPL;
	unsigned int opc;

	nvmeIOCmd = (NVME_IO_COMMAND*)nvmeCmd->cmdDword;

	opc = (unsigned int)nvmeIOCmd->OPC;

	switch(opc)
	{
		case IO_NVM_FLUSH:
		{
			PRINT("IO Flush Command\r\n");
			nvmeCPL.dword[0] = 0;
			nvmeCPL.specific = 0x0;
			set_auto_nvme_cpl(nvmeCmd->cmdSlotTag, nvmeCPL.specific, nvmeCPL.statusFieldWord);
			break;
		}
		case IO_NVM_WRITE:
		{
			PRINT("IO Write Command\r\n");
			handle_nvme_io_write(nvmeCmd->cmdSlotTag, nvmeIOCmd);
			break;
		}
		case IO_NVM_READ:
		{
			PRINT("IO Read Command\r\n");
			handle_nvme_io_read(nvmeCmd->cmdSlotTag, nvmeIOCmd);
			break;
		}
		case IO_NVM_WRITE_ZEROS:
		{
			PRINT("IO Write Zeros Command\r\n");
			nvmeCPL.dword[0] = 0;
			nvmeCPL.specific = 0x0;
			set_auto_nvme_cpl(nvmeCmd->cmdSlotTag, nvmeCPL.specific, nvmeCPL.statusFieldWord);
			break;
		}
		// NVMe commands for CXL-SSD
		/*
		case IO_PRINT_STATS:
		{
				handle_nvme_io_trace_push(nvmeCmd->cmdSlotTag, nvmeIOCmd);
				break;
		}*/
		case IO_NVM_SINGLE_TRACE:
		{
			PRINT("IO NVM_SINGLE_TRACE\r\n");
				handle_nvme_single_trace(nvmeCmd->cmdSlotTag, nvmeIOCmd);
				break;
		}
		case IO_NVM_WRITE_PAGE:
		{
				PRINT("IO_NVM_WRITE_PAGE\r\n");
				handle_nvme_write_page(nvmeCmd->cmdSlotTag, nvmeIOCmd);
				break;
		}
		case IO_PRINT_STATS:
		{
				handle_stats(nvmeCmd->cmdSlotTag, nvmeIOCmd);
				break;
		}
		default:
		{
			xil_printf("Not Support IO Command OPC: 0x%X\r\n", opc);
			ASSERT(0);
			break;
		}
	}

	if (opc < 0xA0)
		ReqTransSliceToLowLevel();

#if (__IO_CMD_DONE_MESSAGE_PRINT)
    xil_printf("OPC = 0x%X\r\n", nvmeIOCmd->OPC);
    xil_printf("PRP1[63:32] = 0x%X, PRP1[31:0] = 0x%X\r\n", nvmeIOCmd->PRP1[1], nvmeIOCmd->PRP1[0]);
    xil_printf("PRP2[63:32] = 0x%X, PRP2[31:0] = 0x%X\r\n", nvmeIOCmd->PRP2[1], nvmeIOCmd->PRP2[0]);
    xil_printf("dword10 = 0x%X\r\n", nvmeIOCmd->dword10);
    xil_printf("dword11 = 0x%X\r\n", nvmeIOCmd->dword11);
    xil_printf("dword12 = 0x%X\r\n", nvmeIOCmd->dword12);
#endif
}

