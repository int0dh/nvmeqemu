/*
 * Copyright (c) 2011 Intel Corporation
 *
 * by
 *    Maciej Patelczyk <mpatelcz@gkslx007.igk.intel.com>
 *    Krzysztof Wierzbicki <krzysztof.wierzbicki@intel.com>
 *    Patrick Porlan <patrick.porlan@intel.com>
 *    Nisheeth Bhat <nisheeth.bhat@intel.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>
 */


#include "nvme.h"
#include "nvme_debug.h"

static uint32_t adm_cmd_del_sq(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe);
static uint32_t adm_cmd_alloc_sq(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe);
static uint32_t adm_cmd_del_cq(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe);
static uint32_t adm_cmd_alloc_cq(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe);
static uint32_t adm_cmd_get_log_page(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe);
static uint32_t adm_cmd_identify(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe);
static uint32_t adm_cmd_abort(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe);
static uint32_t adm_cmd_set_features(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe);
static uint32_t adm_cmd_get_features(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe);
static uint32_t adm_cmd_async_ev_req(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe);

typedef uint32_t adm_command_func(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe);

static adm_command_func * const adm_cmds_funcs[] = {
    [NVME_ADM_CMD_DELETE_SQ] = adm_cmd_del_sq,
    [NVME_ADM_CMD_CREATE_SQ] = adm_cmd_alloc_sq,
    [NVME_ADM_CMD_GET_LOG_PAGE] = adm_cmd_get_log_page,
    [NVME_ADM_CMD_DELETE_CQ] = adm_cmd_del_cq,
    [NVME_ADM_CMD_CREATE_CQ] = adm_cmd_alloc_cq,
    [NVME_ADM_CMD_IDENTIFY] = adm_cmd_identify,
    [NVME_ADM_CMD_ABORT] = adm_cmd_abort,
    [NVME_ADM_CMD_SET_FEATURES] = adm_cmd_set_features,
    [NVME_ADM_CMD_GET_FEATURES] = adm_cmd_get_features,
    [NVME_ADM_CMD_ASYNC_EV_REQ] = adm_cmd_async_ev_req,
    [NVME_ADM_CMD_LAST] = NULL,
};

uint8_t nvme_admin_command(NVMEState *n, NVMECmd *sqe, NVMECQE *cqe)
{
    uint8_t ret = NVME_SC_DATA_XFER_ERROR;

    NVMEStatusField *sf = (NVMEStatusField *) &cqe->status;
    adm_command_func *f;

    if ((sqe->opcode >= NVME_ADM_CMD_LAST) ||
        (!adm_cmds_funcs[sqe->opcode])) {
        sf->sc = NVME_SC_INVALID_OPCODE;
    } else {
        f = adm_cmds_funcs[sqe->opcode];
        ret = f(n, sqe, cqe);
    }
    return ret;
}

static uint32_t adm_check_cqid(NVMEState *n, uint16_t cqid)
{
    LOG_NORM("kw q: check if exists cqid %d", cqid);
    /* If queue is allocated dma_addr!=NULL and has the same ID */
    if (cqid >= NVME_MAX_QID) {
        return FAIL;
    } else if (n->cq[cqid].dma_addr && n->cq[cqid].id == cqid) {
        return 0;
    } else {
      return FAIL;
    }
}

static uint32_t adm_check_sqid(NVMEState *n, uint16_t sqid)
{
    /* If queue is allocated dma_addr!=NULL and has the same ID */
    if (sqid >= NVME_MAX_QID) {
        return FAIL;
    } else if (n->sq[sqid].dma_addr && n->sq[sqid].id == sqid) {
        return 0;
    } else {
        return FAIL;
    }
}

static uint16_t adm_get_sq(NVMEState *n, uint16_t sqid)
{
    if (sqid >= NVME_MAX_QID) {
        return NVME_MAX_QID;
    } else if (n->sq[sqid].dma_addr && n->sq[sqid].id == sqid) {
        return sqid;
    } else {
        return NVME_MAX_QID;
    }
}

static uint16_t adm_get_cq(NVMEState *n, uint16_t cqid)
{
    if (cqid >= NVME_MAX_QID) {
        return NVME_MAX_QID;
    } else if (n->cq[cqid].dma_addr && n->cq[cqid].id == cqid) {
        return cqid;
    } else {
        return NVME_MAX_QID;
    }

}

/* FIXME: For now allow only empty queue. */
static uint32_t adm_cmd_del_sq(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe)
{
    /* If something is in the queue then abort all pending messages.
     * TBD: What if there is no space in cq? */
    NVMEAdmCmdDeleteSQ *c = (NVMEAdmCmdDeleteSQ *)cmd;
    NVMEIOCQueue *cq;
    NVMEIOSQueue *sq;
    NVMEStatusField *sf = (NVMEStatusField *)&cqe->status;
    uint16_t i;
    sf->sc = NVME_SC_SUCCESS;

    LOG_NORM("%s(): called with QID:%d", __func__, c->qid);

    if (!n) {
        return FAIL;
    }
    /* Log's done to do unit testing */
    LOG_DBG("Delete SQ command for SQID: %u", c->qid);

    if (cmd->opcode != NVME_ADM_CMD_DELETE_SQ) {
        LOG_NORM("%s(): Invalid opcode %d", __func__, cmd->opcode);
        sf->sc = NVME_SC_INVALID_OPCODE;
        return FAIL;
    }

    if (c->qid == 0 || c->qid > NVME_MAX_QID) {
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        sf->sc = NVME_INVALID_QUEUE_IDENTIFIER;
        return FAIL;
    } else if (c->nsid != 0) {
        LOG_NORM("%s():Invalid namespace", __func__);
        sf->sc = NVME_SC_INVALID_NAMESPACE;
        return FAIL;
    }

    i = adm_get_sq(n, c->qid);
    if (i == NVME_MAX_QID) {
        LOG_NORM("No such queue: SQ %d", c->qid);
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        sf->sc = NVME_INVALID_QUEUE_IDENTIFIER;
        return FAIL;
    }
    sq = &n->sq[i];
    if (sq->tail != sq->head) {
        /* Queue not empty */
    }

    if (sq->cq_id != NVME_MAX_QID) {
        cq = &n->cq[sq->cq_id];
        if (cq->id == NVME_MAX_QID) {
            /* error */
            sf->sct = NVME_SCT_CMD_SPEC_ERR;
            sf->sc = NVME_INVALID_QUEUE_IDENTIFIER;
            return FAIL;
        }

        if (!cq->usage_cnt) {
            /* error FIXME */
        }

        cq->usage_cnt--;
    }

    sq->id = sq->cq_id = NVME_MAX_QID;
    sq->head = sq->tail = 0;
    sq->size = 0;
    sq->prio = 0;
    sq->phys_contig = 0;
    sq->dma_addr = 0;

    return 0;
}

static uint32_t adm_cmd_alloc_sq(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe)
{
    NVMEAdmCmdCreateSQ *c = (NVMEAdmCmdCreateSQ *)cmd;
    NVMEIOSQueue *sq;
    uint16_t *mqes;
    NVMEStatusField *sf = (NVMEStatusField *)&cqe->status;
    sf->sc = NVME_SC_SUCCESS;

    LOG_NORM("%s(): called", __func__);

    if (!n) {
        return FAIL;
    }

    if (cmd->opcode != NVME_ADM_CMD_CREATE_SQ) {
        LOG_NORM("%s(): Invalid opcode %d", __func__, cmd->opcode);
        sf->sc = NVME_SC_INVALID_OPCODE;
        return FAIL;
    }

    /* Log's done to do unit testing */
    LOG_DBG("Create SQ command for QID: %u", c->qid);
    LOG_DBG("Create SQ command with Qsize: %u", c->qsize);
    LOG_DBG("Create SQ command with PC bit: %u", c->pc);
    LOG_DBG("Create SQ command with unique command ID: %u", c->cid);
    LOG_DBG("Create SQ command with PRP1: %lu", c->prp1);
    LOG_DBG("Create SQ command with PRP2: %lu", c->prp2);
    LOG_DBG("Create SQ command is assoc with CQID: %u", c->cqid);

    if (c->qid == 0 || c->qid >= NVME_MAX_QID) {
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        sf->sc = NVME_INVALID_QUEUE_IDENTIFIER;
        LOG_NORM("%s():NVME_INVALID_QUEUE_IDENTIFIER in Command", __func__);
        return FAIL;
    } else if (c->cqid == 0) {
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        sf->sc = NVME_COMPLETION_QUEUE_INVALID;
        LOG_NORM("%s():CQID should not be 0", __func__);
        return FAIL;
    } else if (c->nsid != 0) {
        LOG_NORM("%s():Invalid namespace identifier", __func__);
        sf->sc = NVME_SC_INVALID_NAMESPACE;
        return FAIL;
    }
    /* Invalid SQID, exists*/
    if (!adm_check_sqid(n, c->qid)) {
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        sf->sc = NVME_INVALID_QUEUE_IDENTIFIER;
        LOG_NORM("%s():SQID in command already allocated/invalid ID", __func__);
        return FAIL;
    }

    /* Corresponding CQ exists?  if not return error */
    if (adm_check_cqid(n, c->cqid)) {
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        sf->sc = NVME_COMPLETION_QUEUE_INVALID;
        LOG_NORM("%s():CQID in command not allocated", __func__);
        return FAIL;
    }
    mqes = (uint16_t *) n->cntrl_reg;

    /* Queue Size */
    if (c->qsize > (*mqes + 1)) {
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        sf->sc = NVME_MAX_QUEUE_SIZE_EXCEEDED;
        LOG_NORM("%s():MQES %u exceeded", __func__, (*mqes + 1));
        return FAIL;
    }

    if ((c->pc == 0) && (*(mqes + 0x01) & 0x01)) {
        LOG_NORM("%s():CAP.CQR set to 1.Thus\
            controller supports only contiguous IO queues", __func__);
        sf->sc = NVME_SC_INVALID_FIELD;
        return FAIL;
    }

    /* In PRP1 is DMA address. Chapter 5.4, Figure 36 */
    if (c->prp1 == 0) {
        LOG_NORM("%s():PRP1 field is invalid", __func__);
        sf->sc = NVME_SC_INVALID_FIELD;
        return FAIL;
    }

    sq = &n->sq[c->qid];
    sq->id = c->qid;
    sq->size = c->qsize;
    sq->phys_contig = c->pc;
    sq->cq_id = c->cqid;
    sq->prio = c->qprio;
    sq->dma_addr = c->prp1;

    LOG_DBG("sq->id %d, sq->dma_addr 0x%x, %lu",
        sq->id, (unsigned int)sq->dma_addr,
        (unsigned long int)sq->dma_addr);

    /* Mark CQ as used by this queue. */
    n->cq[adm_get_cq(n, c->cqid)].usage_cnt++;

    return 0;
}

static uint32_t adm_cmd_del_cq(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe)
{
    NVMEAdmCmdDeleteCQ *c = (NVMEAdmCmdDeleteCQ *)cmd;
    NVMEIOCQueue *cq;
    NVMEStatusField *sf = (NVMEStatusField *)&cqe->status;
    uint16_t i;
    sf->sc = NVME_SC_SUCCESS;

    LOG_NORM("%s(): called", __func__);

    if (!n) {
        return FAIL;
    }

    /* Log's done to do unit testing */
    LOG_DBG("Delete CQ command for CQID: %u", c->qid);

    if (cmd->opcode != NVME_ADM_CMD_DELETE_CQ) {
        LOG_NORM("%s(): Invalid opcode %d\n", __func__, cmd->opcode);
        sf->sc = NVME_SC_INVALID_OPCODE;
        return FAIL;
    }

    if (c->qid == 0 || c->qid > NVME_MAX_QID) {
        LOG_NORM("%s():Invalid Queue ID %d", __func__, c->qid);
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        sf->sc = NVME_INVALID_QUEUE_IDENTIFIER;
        return FAIL;
    } else if (c->nsid != 0) {
        LOG_NORM("%s():Invalid namespace", __func__);
        sf->sc = NVME_SC_INVALID_NAMESPACE;
        return FAIL;
    }


    i = adm_get_cq(n, c->qid);
    if (i == NVME_MAX_QID) {
        LOG_NORM("No such queue: CQ %d", c->qid);
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        sf->sc = NVME_INVALID_QUEUE_IDENTIFIER;
        return FAIL;
    }
    cq = &n->cq[i];

    if (cq->tail != cq->head) {
        /* Queue not empty */
        /* error */
    }

    /* Do not allow to delete CQ when some SQ is pointing on it. */
    if (cq->usage_cnt) {
        LOG_ERR("Error. Some sq are still connected to CQ %d", c->qid);
        sf->sc = NVME_SC_INVALID_FIELD;
        return NVME_SC_INVALID_FIELD;
    }

    cq->id = NVME_MAX_QID;
    cq->head = cq->tail = 0;
    cq->size = 0;
    cq->irq_enabled = 0;
    cq->vector = 0;
    cq->dma_addr = 0;
    cq->phys_contig = 0;

    return 0;
}

static uint32_t adm_cmd_alloc_cq(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe)
{
    NVMEAdmCmdCreateCQ *c = (NVMEAdmCmdCreateCQ *)cmd;
    NVMEIOCQueue *cq;
    uint16_t *mqes;
    NVMEStatusField *sf = (NVMEStatusField *)&cqe->status;
    sf->sc = NVME_SC_SUCCESS;

    LOG_NORM("%s(): called", __func__);

    if (!n) {
        return FAIL;
    }
    /* Log's done to do unit testing */
    LOG_DBG("Create CQ command for QID: %u", c->qid);
    LOG_DBG("Create CQ command with Qsize: %u", c->qsize);
    LOG_DBG("Create CQ command with PC bit: %u", c->pc);
    LOG_DBG("Create CQ command with unique command ID: %u", c->cid);
    LOG_DBG("Create CQ command with PRP1: %lu", c->prp1);
    LOG_DBG("Create CQ command with PRP2: %lu", c->prp2);

    if (cmd->opcode != NVME_ADM_CMD_CREATE_CQ) {
        LOG_NORM("%s(): Invalid opcode %d", __func__, cmd->opcode);
        sf->sc = NVME_SC_INVALID_OPCODE;
        return FAIL;
    }

    if (c->qid == 0 || c->qid >= NVME_MAX_QID) {
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        sf->sc = NVME_INVALID_QUEUE_IDENTIFIER;
        LOG_NORM("%s():NVME_INVALID_QUEUE_IDENTIFIER in Command", __func__);
        return FAIL;
    } else if (c->nsid != 0) {
        LOG_NORM("%s():Invalid namespace", __func__);
        sf->sc = NVME_SC_INVALID_NAMESPACE;
        return FAIL;
    }

    /* check if CQ exists., If yes return error */
    if (!adm_check_cqid(n, c->qid)) {
        LOG_NORM("%s():Invalid CQ ID %d\n", __func__, c->qid);
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        sf->sc = NVME_INVALID_QUEUE_IDENTIFIER;
        return FAIL;
    }

    mqes = (uint16_t *) n->cntrl_reg;

    /* Queue Size */
    if (c->qsize > (*mqes + 1)) {
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        sf->sc = NVME_MAX_QUEUE_SIZE_EXCEEDED;
        LOG_NORM("%s():MQES %u exceeded", __func__, (*mqes + 1));
        return FAIL;
    }

    if ((c->pc == 0) && (*(mqes + 0x01) & 0x01)) {
        LOG_ERR("CAP.CQR set to 1");
        sf->sc = NVME_SC_INVALID_FIELD;
        return FAIL;
    }
    /* In PRP1 is DMA address. */
    if (c->prp1 == 0) {
        LOG_NORM("%s():PRP1 address is invalid", __func__);
        sf->sc = NVME_SC_INVALID_FIELD;
        return FAIL;
    }

    if (c->iv > n->dev.msix_entries_nr - 1 && IS_MSIX(n)) {
        /* TODO : checks for MSI too */
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        sf->sc = NVME_INVALID_INTERRUPT_VECTOR;
        return FAIL;
    }

    cq = &n->cq[c->qid];

    cq->id = c->qid;
    cq->dma_addr = c->prp1;
    cq->irq_enabled = c->ien;
    cq->vector = c->iv;
    cq->phase_tag = 1;

    LOG_DBG("kw q: cq[%d] phase_tag   %d", cq->id, cq->phase_tag);
    LOG_DBG("kw q: msix vector. cq[%d] vector %d irq_enabled %d",
                     cq->id, cq->vector, cq->irq_enabled);
    cq->size = c->qsize;
    cq->phys_contig = c->pc;

    return 0;
}

static uint32_t adm_cmd_get_log_page(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe)
{
    NVMEStatusField *sf = (NVMEStatusField *)&cqe->status;
    sf->sc = NVME_SC_SUCCESS;

    if (cmd->opcode != NVME_ADM_CMD_GET_LOG_PAGE) {
        LOG_NORM("%s(): Invalid opcode %d", __func__, cmd->opcode);
        sf->sc = NVME_SC_INVALID_OPCODE;
        return FAIL;
    }

    LOG_NORM("%s(): called", __func__);

    return 0;
}

static uint32_t adm_cmd_id_ctrl(NVMEState *n, NVMECmd *cmd)
{

    uint32_t len;
    LOG_NORM("%s(): copying %lu data into addr %lu",
        __func__, sizeof(*n->idtfy_ctrl), cmd->prp1);

    len = PAGE_SIZE - (cmd->prp1 % PAGE_SIZE);
    nvme_dma_mem_write(cmd->prp1, (uint8_t *) n->idtfy_ctrl, len);
    if (len != sizeof(*(n->idtfy_ctrl))) {
        nvme_dma_mem_write(cmd->prp2,
            (uint8_t *) ((uint8_t *) n->idtfy_ctrl + len),
                (sizeof(*(n->idtfy_ctrl)) - len));
    }
    return 0;
}

/* Needs to be checked if this namespace exists. */
static uint32_t adm_cmd_id_ns(NVMEState *n, NVMECmd *cmd)
{
    uint32_t len;
    LOG_NORM("%s(): called", __func__);

    LOG_DBG("Current Namespace utilization: %lu",
        n->disk[(cmd->nsid - 1)].idtfy_ns->nuse);

    len = PAGE_SIZE - (cmd->prp1 % PAGE_SIZE);
    nvme_dma_mem_write(cmd->prp1,
        (uint8_t *) n->disk[(cmd->nsid - 1)].idtfy_ns, len);
    if (len != sizeof(*(n->disk[(cmd->nsid - 1)].idtfy_ns))) {
        nvme_dma_mem_write(cmd->prp2,
            (uint8_t *) ((uint8_t *) n->disk[(cmd->nsid - 1)].idtfy_ns + len),
                (sizeof(*(n->disk[(cmd->nsid - 1)].idtfy_ns)) - len));
    }
    return 0;
}

static uint32_t adm_cmd_identify(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe)
{
    NVMEAdmCmdIdentify *c = (NVMEAdmCmdIdentify *)cmd;
    uint8_t ret;
    NVMEStatusField *sf = (NVMEStatusField *)&cqe->status;
    sf->sc = NVME_SC_SUCCESS;

    LOG_NORM("%s(): called", __func__);

    if (cmd->opcode != NVME_ADM_CMD_IDENTIFY) {
        LOG_NORM("%s(): Invalid opcode %d", __func__, cmd->opcode);
        sf->sc = NVME_SC_INVALID_OPCODE;
        return FAIL;
    }

    if (c->prp1 == 0) {
        LOG_NORM("%s(): prp1 absent", __func__);
        sf->sc = NVME_SC_INVALID_FIELD;
        return FAIL;
    }

    /* Construct some data and copy it to the addr.*/
    if (c->cns == NVME_IDENTIFY_CONTROLLER) {
        if (c->nsid != 0) {
            LOG_NORM("%s(): Invalid Namespace ID", __func__);
            sf->sc = NVME_SC_INVALID_NAMESPACE;
            return FAIL;
        }
        ret = adm_cmd_id_ctrl(n, cmd);
    } else {
        /* Check for name space */
        if (c->nsid == 0 || (c->nsid > n->idtfy_ctrl->nn)) {
            LOG_NORM("%s(): Invalid Namespace ID", __func__);
            sf->sc = NVME_SC_INVALID_NAMESPACE;
            return FAIL;
        }
        ret = adm_cmd_id_ns(n, cmd);
    }
    if (ret) {
        sf->sc = NVME_SC_INTERNAL;
    }
    return 0;
}


/* 5.1 Abort command
 * The Abort command is used to cancel/abort a specific I/O command previously
 * issued to the Admin or an I/O Submission Queue.Host software may have
 * multiple Abort commands outstanding, subject to the constraints of the
 * Abort Command Limit indicated in the Identify Controller data structure.
 * An abort is a best effort command; the command to abort may have already
 * completed, currently be in execution, or may be deeply queued.
 * It is implementation specific if/when a controller chooses to complete
 * the command with an error (i.e., Requested Command to Abort Not Found)
 * when the command to abort is not found.
*/
static uint32_t adm_cmd_abort(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe)
{
    NVMEAdmCmdAbort *c = (NVMEAdmCmdAbort *)cmd;
    NVMEIOSQueue *sq;
    uint16_t i, tmp;
    target_phys_addr_t addr;
    NVMECmd sqe;
    NVMEStatusField *sf = (NVMEStatusField *)&cqe->status;
    sf->sc = NVME_SC_SUCCESS;

    LOG_NORM("%s(): called", __func__);

    if (cmd->opcode != NVME_ADM_CMD_ABORT) {
        LOG_NORM("%s(): Invalid opcode %d", __func__, cmd->opcode);
        sf->sc = NVME_SC_INVALID_OPCODE;
        return FAIL;
    } else if (c->nsid != 0) {
        LOG_NORM("%s():Invalid namespace", __func__);
        sf->sc = NVME_SC_INVALID_NAMESPACE;
        return FAIL;
    }

    if (c->sqid >= NVME_MAX_QID) {
        sf->sc = NVME_SC_INVALID_FIELD;
        return FAIL;
    }

    if (c->sqid == ASQ_ID) {
        LOG_NORM("Abort command for admin queue is not supported");
        /* cmd_specific = NVME_CMD_ERR_ABORT_CMD */
        /* cqe->status = NVME_SC_SUCCESS << 1; */
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        sf->sc = NVME_REQ_CMD_TO_ABORT_NOT_FOUND;
        return FAIL;
    }

    i = adm_get_sq(n, c->sqid);
    if (i == NVME_MAX_QID) {
        /* Failed - no SQ found*/
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        sf->sc = NVME_REQ_CMD_TO_ABORT_NOT_FOUND;
        return FAIL;
    }

    if (n->abort == NVME_ABORT_COMMAND_LIMIT) {
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        sf->sc = NVME_ABORT_CMD_LIMIT_EXCEEDED;
        return FAIL;
    }

    sq = &n->sq[i];

    for (i = 0; i < NVME_ABORT_COMMAND_LIMIT; i++) {
        if (sq->abort_cmd_id[i] == NVME_EMPTY) {
            break;
        }
    }

    if (i == NVME_ABORT_COMMAND_LIMIT) {
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        sf->sc = NVME_ABORT_CMD_LIMIT_EXCEEDED;
        return FAIL;
    }

    tmp = i;
    i = sq->head;
    while (i != sq->tail) {

        addr = sq->dma_addr + i * sizeof(sqe);
        nvme_dma_mem_read(addr, (uint8_t *)&sqe, sizeof(sqe));

        if (sqe.cid == c->cmdid) {
            sq->abort_cmd_id[tmp] = c->cmdid;
            n->abort++;
            break;
        }

        i++;
        if (i == sq->size) {
            i = 0;
        }
    }

    if (sq->abort_cmd_id[tmp] == NVME_EMPTY) {
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        sf->sc = NVME_REQ_CMD_TO_ABORT_NOT_FOUND;
        return FAIL;
    }

    return 0;
}

static uint32_t do_features(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe)
{
    NVMEAdmCmdFeatures *sqe = (NVMEAdmCmdFeatures *)cmd;
    NVMEStatusField *sf = (NVMEStatusField *)&cqe->status;
    sf->sc = NVME_SC_SUCCESS;

    switch (sqe->fid) {
    case NVME_FEATURE_ARBITRATION:
        if (sqe->opcode == NVME_ADM_CMD_SET_FEATURES) {
            n->feature.arbitration = sqe->cdw11;
        } else {
            cqe->cmd_specific = n->feature.arbitration;
        }
        break;

    case NVME_FEATURE_POWER_MANAGEMENT:
        if (sqe->opcode == NVME_ADM_CMD_SET_FEATURES) {
            n->feature.power_management = sqe->cdw11;
        } else {
            cqe->cmd_specific = n->feature.power_management;
        }
        break;

    case NVME_FEATURE_LBA_RANGE_TYPE:
        LOG_NORM("NVME_FEATURE_LBA_RANGE_TYPE not supported yet");
        break;

    case NVME_FEATURE_TEMPERATURE_THRESHOLD:
        if (sqe->opcode == NVME_ADM_CMD_SET_FEATURES) {
            n->feature.temperature_threshold = sqe->cdw11;
        } else {
            cqe->cmd_specific = n->feature.temperature_threshold;
        }
        break;

    case NVME_FEATURE_ERROR_RECOVERY:
        if (sqe->opcode == NVME_ADM_CMD_SET_FEATURES) {
            n->feature.error_recovery = sqe->cdw11;
        } else {
            cqe->cmd_specific = n->feature.error_recovery;
        }
        break;

    case NVME_FEATURE_VOLATILE_WRITE_CACHE:
        if (sqe->opcode == NVME_ADM_CMD_SET_FEATURES) {
            n->feature.volatile_write_cache = sqe->cdw11;
        } else {
            cqe->cmd_specific = n->feature.volatile_write_cache;
        }
        break;

    case NVME_FEATURE_NUMBER_OF_QUEUES:
        if (sqe->opcode == NVME_ADM_CMD_SET_FEATURES) {
            n->feature.number_of_queues = sqe->cdw11;
        } else {
            cqe->cmd_specific = n->feature.number_of_queues;
        }
        break;

    case NVME_FEATURE_INTERRUPT_COALESCING:
        if (sqe->opcode == NVME_ADM_CMD_SET_FEATURES) {
            n->feature.interrupt_coalescing = sqe->cdw11;
        } else {
            cqe->cmd_specific = n->feature.interrupt_coalescing;
        }
        break;

    case NVME_FEATURE_INTERRUPT_VECTOR_CONF:
        if (sqe->opcode == NVME_ADM_CMD_SET_FEATURES) {
            n->feature.interrupt_vector_configuration = sqe->cdw11;
        } else {
            cqe->cmd_specific =
                n->feature.interrupt_vector_configuration;
        }
        break;

    case NVME_FEATURE_WRITE_ATOMICITY:
        if (sqe->opcode == NVME_ADM_CMD_SET_FEATURES) {
            n->feature.write_atomicity = sqe->cdw11;
        } else {
            cqe->cmd_specific = n->feature.write_atomicity;
        }
        break;

    case NVME_FEATURE_ASYNCHRONOUS_EVENT_CONF:
        if (sqe->opcode == NVME_ADM_CMD_SET_FEATURES) {
            n->feature.asynchronous_event_configuration
                                 = sqe->cdw11;
        } else {
            cqe->cmd_specific =
                n->feature.asynchronous_event_configuration;
        }
        break;

    case NVME_FEATURE_SOFTWARE_PROGRESS_MARKER: /* Set Features only*/
        if (sqe->opcode == NVME_ADM_CMD_GET_FEATURES) {
            cqe->cmd_specific =
                n->feature.software_progress_marker;
        }
        break;

    default:
        LOG_NORM("Unknown feature ID: %d", sqe->fid);
        sf->sc = NVME_SC_INVALID_FIELD;
        break;
    }

    return 0;
}

static uint32_t adm_cmd_set_features(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe)
{
    NVMEStatusField *sf = (NVMEStatusField *)&cqe->status;
    uint32_t res;

    if (cmd->opcode != NVME_ADM_CMD_SET_FEATURES) {
        LOG_NORM("%s(): Invalid opcode %d", __func__, cmd->opcode);
        sf->sc = NVME_SC_INVALID_OPCODE;
        return FAIL;
    }

    res = do_features(n, cmd, cqe);

    LOG_NORM("%s(): called", __func__);
    return res;
}

static uint32_t adm_cmd_get_features(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe)
{
    NVMEStatusField *sf = (NVMEStatusField *)&cqe->status;
    uint32_t res;

    if (cmd->opcode != NVME_ADM_CMD_GET_FEATURES) {
        LOG_NORM("%s(): Invalid opcode %d", __func__, cmd->opcode);
        sf->sc = NVME_SC_INVALID_OPCODE;
        return FAIL;
    }

    res = do_features(n, cmd, cqe);

    LOG_NORM("%s(): called", __func__);
    return res;
}

static uint32_t adm_cmd_async_ev_req(NVMEState *n, NVMECmd *cmd, NVMECQE *cqe)
{
    /* NVMEAdmCmdAsyncEvRq *c = (NVMEAdmCmdAsyncEvRq *)cmd; */
    NVMEStatusField *sf = (NVMEStatusField *)&cqe->status;
    sf->sc = NVME_SC_SUCCESS;

    if (cmd->opcode != NVME_ADM_CMD_ASYNC_EV_REQ) {
        LOG_NORM("%s(): Invalid opcode %d", __func__, cmd->opcode);
        sf->sc = NVME_SC_INVALID_OPCODE;
        return FAIL;
    }

    LOG_NORM("%s(): called", __func__);
    return 0;
}