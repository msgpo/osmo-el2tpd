#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <arpa/inet.h>

#include <openssl/engine.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

#include <osmocom/core/msgb.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/select.h>
#include <osmocom/core/socket.h>
#include <osmocom/core/fsm.h>

#include "l2tp_protocol.h"
#include "l2tpd.h"
#include "l2tpd_data.h"
#include "l2tpd_fsm.h"
#include "l2tpd_packet.h"
#include "l2tpd_lapd.h"
#include "l2tpd_logging.h"
#include "l2tpd_socket.h"

struct l2tpd_instance *l2i;
/* FIXME: global static instance */

static int l2tp_ip_read_cb(struct osmo_fd *ofd, unsigned int what)
{
	struct msgb *msg = l2tp_msgb_alloc();
	struct sockaddr ss;
	socklen_t ss_len = sizeof(ss);
	int rc;

	/* actually read the message from the raw IP socket */
	rc = recvfrom(ofd->fd, msg->data, msg->data_len, 0,
			(struct sockaddr *) &ss, &ss_len);
	if (rc < 0) {
		LOGP(DL2TP, LOGL_ERROR, "recievefrom failed %s\n", strerror(errno));
		return rc;
	}
	msgb_put(msg, rc);
	msg->l1h = msg->data; /* l1h = ip header */

	msgb_pull(msg, 20); /* IPv4 header. FIXME: Should depend on the family */
	msg->l2h = msg->data;
	msg->dst = &ss;

	rc = l2tp_rcvmsg(msg);
	msgb_free(msg);

	return rc;
}

static int l2tpd_instance_start(struct l2tpd_instance *li)
{
	int rc;
	uint8_t dscp = 0xb8;

	INIT_LLIST_HEAD(&li->connections);

	li->l2tp_ofd.when = BSC_FD_READ;
	li->l2tp_ofd.cb = l2tp_ip_read_cb;
	li->l2tp_ofd.data = li;

	rc = osmo_sock_init_ofd(&li->l2tp_ofd, AF_INET, SOCK_RAW,
				IPPROTO_L2TP, li->cfg.bind_ip, 0, 0);
	if (rc < 0)
		return rc;

	setsockopt(li->l2tp_ofd.fd, IPPROTO_IP, IP_TOS,
		    &dscp, sizeof(dscp));

	return 0;
}

int main(int argc, char **argv)
{
	int rc;

	void *tall_l2tp_ctx = talloc_named_const(NULL, 0, "l2tpd");

	l2tpd_log_init();

	/* register fsms */
	osmo_fsm_register(&l2tp_cc_fsm);
	osmo_fsm_register(&l2tp_ic_fsm);
	osmo_fsm_register(&l2tp_conf_fsm);

	l2i = talloc_zero(tall_l2tp_ctx, struct l2tpd_instance);
	l2i->cfg.bind_ip = "0.0.0.0";
	l2i->cfg.rsl_oml_path = "/tmp/rsl_oml";
	l2i->cfg.pgsl_path = "/tmp/pgsl";
	l2i->cfg.trau_path = "/tmp/trau";
	/* connection id starts with 1 */
	l2i->next_l_cc_id = 1;
	/* session id starts with 1 */
	l2i->next_l_sess_id = 1;

	rc = l2tpd_instance_start(l2i);
	if (rc < 0)
		exit(1);

	l2tp_socket_init(&l2i->rsl_oml.state, l2i->cfg.rsl_oml_path, 100, DL2TP);
	l2tp_socket_init(&l2i->trau.state, l2i->cfg.trau_path, 100, DL2TP);
	l2tp_socket_init(&l2i->pgsl.state, l2i->cfg.pgsl_path, 100, DL2TP);

	l2tp_set_read_callback(&l2i->rsl_oml.state, unix_rsl_oml_cb);
	l2tp_set_read_callback(&l2i->pgsl.state, unix_rsl_oml_cb);
	l2tp_set_read_callback(&l2i->trau.state, unix_rsl_oml_cb);

	while (1) {
		osmo_select_main(0);
	}
}
