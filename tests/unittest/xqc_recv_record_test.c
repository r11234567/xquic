/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "xqc_recv_record_test.h"
#include "src/transport/xqc_packet.h"
#include "src/transport/xqc_recv_record.h"
#include <string.h>
#include "xqc_test_helpers.h"
#include "src/transport/xqc_multipath.h"
#include "src/transport/xqc_packet_out.h"
#include "src/transport/xqc_send_ctl.h"
#include "src/transport/xqc_timer.h"
extern void xqc_timer_ack_timeout(xqc_timer_type_t type, xqc_usec_t now,
    void *user_data);
#include <CUnit/CUnit.h>
#include <stdio.h>

void
xqc_test_recv_record()
{
    xqc_list_head_t *pos;
    xqc_pktno_range_node_t *pnode;
    xqc_recv_record_t record;
    memset(&record, 0, sizeof(xqc_recv_record_t));
    xqc_init_list_head(&record.list_head);

    xqc_recv_record_add(&record, 0);
    xqc_recv_record_add(&record, 1);
    xqc_recv_record_add(&record, 10);
    xqc_recv_record_add(&record, 2);

    xqc_list_for_each(pos, &record.list_head) {
        pnode = xqc_list_entry(pos, xqc_pktno_range_node_t, list);
        //printf("low:%llu, high=%llu\n", pnode->pktno_range.low, pnode->pktno_range.high);
    }

    /* printf("largest=%llu\n", xqc_recv_record_largest(&record)); */
    CU_ASSERT(10 == xqc_recv_record_largest(&record));


    xqc_recv_record_del(&record, 5);
    xqc_list_for_each(pos, &record.list_head) {
        pnode = xqc_list_entry(pos, xqc_pktno_range_node_t, list);
        //printf("low:%llu, high=%llu\n", pnode->pktno_range.low, pnode->pktno_range.high);
    }

    xqc_recv_record_destroy(&record);
}

/* Internal ACK readiness must not depend on the magnitude of a path ID. */
void
xqc_test_ack_dynamic_path_ids(void)
{
    const uint64_t ids[] = {0, 10, 21, 22, 100, (1ULL << 62) - 1};
    xqc_connection_t *conn = xqc_test_helper_conn_create(NULL);
    CU_ASSERT_PTR_NOT_NULL_FATAL(conn);
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        xqc_path_ctx_t *path = xqc_test_helper_path_synthesize(
            conn, ids[i], XQC_PATH_STATE_ACTIVE);
        CU_ASSERT_PTR_NOT_NULL_FATAL(path);
        xqc_send_ctl_t ctl = {0};
        ctl.ctl_conn = conn;
        ctl.ctl_path = path;
        ctl.path_timer_manager.log = conn->log;
        path->path_send_ctl = &ctl;
        conn->conn_flag |= XQC_CONN_FLAG_CAN_SEND_1RTT;
        conn->conn_settings.ack_frequency = 2;
        ctl.ctl_ack_eliciting_pkt[XQC_PNS_APP_DATA] = 2;
        xqc_maybe_should_ack(conn, path, NULL, XQC_PNS_APP_DATA, 0, 0);
        CU_ASSERT_TRUE(xqc_conn_has_pending_ack(conn));
        path->path_flag &= ~XQC_PATH_FLAG_SHOULD_ACK_01RTT;
        for (int pns = 0; pns < XQC_PNS_N; pns++) {
            CU_ASSERT_FALSE(xqc_conn_has_pending_ack(conn));
            xqc_timer_ack_timeout(XQC_TIMER_ACK_INIT + pns, 0, &ctl);
            CU_ASSERT_TRUE(xqc_conn_has_pending_ack(conn));
            path->path_flag &= ~(XQC_PATH_FLAG_SHOULD_ACK_INIT << pns);
        }
        path->path_send_ctl = NULL;
        xqc_test_helper_path_destroy(path);
    }
    xqc_test_helper_conn_destroy(conn);
}

void
xqc_test_ack_closed_path_isolation(void)
{
    xqc_connection_t *conn = xqc_test_helper_conn_create(NULL);
    CU_ASSERT_PTR_NOT_NULL_FATAL(conn);
    xqc_path_ctx_t *a = xqc_test_helper_path_synthesize(
        conn, 10, XQC_PATH_STATE_ACTIVE);
    xqc_path_ctx_t *b = xqc_test_helper_path_synthesize(
        conn, 100, XQC_PATH_STATE_ACTIVE);
    CU_ASSERT_PTR_NOT_NULL_FATAL(a);
    CU_ASSERT_PTR_NOT_NULL_FATAL(b);
    a->path_flag |= XQC_PATH_FLAG_SHOULD_ACK_01RTT;
    b->path_flag |= XQC_PATH_FLAG_SHOULD_ACK_01RTT;
    a->path_flag &= ~XQC_PATH_FLAG_SHOULD_ACK_01RTT;
    CU_ASSERT_TRUE(xqc_conn_has_pending_ack(conn));
    b->path_state = XQC_PATH_STATE_CLOSED;
    CU_ASSERT_FALSE(xqc_conn_has_pending_ack(conn));
    xqc_test_helper_path_destroy(a);
    xqc_test_helper_path_destroy(b);
    CU_ASSERT_FALSE(xqc_conn_has_pending_ack(conn));
    xqc_test_helper_conn_destroy(conn);
}
