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

#include "l2tp_protocol.h"
#include "l2tpd.h"

/* swap all fields of the l2tp_control header structure */
static void l2tp_hdr_swap(struct l2tp_control_hdr *ch)
{
	ch->ver = ntohs(ch->ver);
	ch->length = ntohs(ch->length);
	ch->ccid = ntohl(ch->ccid);
	ch->Ns = ntohs(ch->Ns);
	ch->Nr = ntohs(ch->Nr);
}

/* a parsed representation of an L2TP AVP */
struct avp_parsed {
	uint16_t vendor_id;
	uint16_t type;
	uint16_t data_len;
	uint8_t m:1,
		h:1;
	uint8_t *data;
};

/* parse the AVP at msg->data + offset and return the new offset */
static int msgb_avp_parse(struct avp_parsed *ap, struct msgb *msg, int offset)
{
	uint32_t msgb_left = msgb_length(msg) - offset;
	struct l2tp_avp_hdr *ah = (struct l2tp_avp_hdr *) msg->data + offset;
	uint16_t avp_len;

	if (sizeof(*ah) > msgb_left) {
		LOGP(DL2TP, LOGL_NOTICE, "AVP Hdr beyond end of msgb\n");
		return -1;
	}
	avp_len = ntohs(ah->m_h_length) & 0x3ff;
	if (avp_len < 6) {
		LOGP(DL2TP, LOGL_NOTICE, "AVP Parse: AVP len < 6\n");
		return -1;
	}
	if (sizeof(*ah) + avp_len > msgb_left) {
		LOGP(DL2TP, LOGL_NOTICE, "AVP Data beyond end of msgb\n");
		return -1;
	}

	ap->vendor_id = ntohs(ah->vendor_id);
	ap->type = ntohs(ah->attr_type);
	ap->data_len = avp_len - sizeof(*ah);
	ap->data = ah->value;
	ap->m = !!(ah->m_h_length & 0x8000);
	ap->h = !!(ah->m_h_length & 0x4000);

	return offset + avp_len;
}

/* store an AVP at the end of the msg */
static int msgb_avp_put(struct msgb *msg, uint16_t vendor_id, uint16_t type,
			const uint8_t *data, uint16_t data_len, bool m_flag)
{
	uint8_t *out;

	if (data_len > 0x3ff - 6) {
		LOGP(DL2TP, LOGL_ERROR, "Data too long for AVP\n");
		return -1;
	}

	msgb_put_u16(msg, (data_len + 6) & 0x3ff | (m_flag ? 0x8000 : 0));
	msgb_put_u16(msg, vendor_id);
	msgb_put_u16(msg, type);
	out = msgb_put(msg, data_len);
	memcpy(out, data, data_len);

	return 6 + data_len;
}

/* store an uint8_t value AVP */
static int msgb_avp_put_u8(struct msgb *msg, uint16_t vendor, uint16_t avp_type,
			   uint8_t val, bool m_flag)
{
	return msgb_avp_put(msg, vendor, avp_type, &val, 1, m_flag);
}

/* store an uint16_t value AVP */
static int msgb_avp_put_u16(struct msgb *msg, uint16_t vendor, uint16_t avp_type,
			    uint16_t val, bool m_flag)
{
	val = htons(val);
	return msgb_avp_put(msg, vendor, avp_type, (uint8_t *)&val, 2, m_flag);
}

/* store an uint32_t value AVP */
static int msgb_avp_put_u32(struct msgb *msg, uint16_t vendor, uint16_t avp_type,
			    uint32_t val, bool m_flag)
{
	val = htonl(val);
	return msgb_avp_put(msg, vendor, avp_type, (uint8_t *)&val, 4, m_flag);
}

/* store a 'message type' AVP */
static int msgb_avp_put_msgt(struct msgb *msg, uint16_t vendor, uint16_t msg_type)
{
	return msgb_avp_put_u16(msg, vendor, AVP_IETF_CTRL_MSG, msg_type, true);
}

static struct msgb *l2tp_msgb_alloc(void)
{
	return msgb_alloc_headroom(1500, 100, "L2TP");
}

static int msgb_avp_put_digest(struct msgb *msg)
{
	/* we simply put a zero-initialized AVP for now and update when
	 * trnasmitting */
	const uint8_t digest_zero[17] = { 0, };
	return msgb_avp_put(msg, VENDOR_IETF, AVP_IETF_MSG_DIGEST,
				digest_zero, sizeof(digest_zero), true);
}

/* E/// L2TP seems to use a static, constant HMAC key */
static const uint8_t digest_key[] = {
	0x7b, 0x60, 0x85, 0xfb, 0xf4, 0x59, 0x33, 0x67,
	0x0a, 0xbc, 0xb0, 0x7a, 0x27, 0xfc, 0xea, 0x5e
};

/* update the message digest inside the AVP of a message */
static int digest_avp_update(struct msgb *msg)
{
	struct l2tp_control_hdr *l2h = msgb_l2tph(msg);
	struct l2tp_avp_hdr *ah = (struct l2tp_avp_hdr *) ((uint8_t *)l2h + sizeof(*l2h));
	uint8_t *hmac_res;
	unsigned int len = ntohs(l2h->length);

	/* Digest AVP header is guaranteed to be the second AVP in a
	 * control message.  First AVP is message type AVP with overall
	 * length of 8 bytes */

	if (ntohs(ah->attr_type) != AVP_IETF_MSG_DIGEST ||
	    ntohs(ah->vendor_id) != VENDOR_IETF ||
	    ntohs(ah->m_h_length) & 0x3FF != 17) {
		LOGP(DL2TP, LOGL_ERROR, "Missing Digest AVP, cannot update\n");
		return -1;
	}

	if (len > msgb_l2tplen(msg)) {
		LOGP(DL2TP, LOGL_ERROR, "");
		return -1;
	}

	DEBUGP(DL2TP, "Tx Message before digest: %s\n", msgb_hexdump(msg));
	/* RFC says HMAC_Hash(shared_key, local_nonce + remote_nonce + control_message),
	 * but ericsson is doning something different without any
	 * local/remote nonce? */
	hmac_res = HMAC(EVP_md5(), digest_key, sizeof(digest_key),
			(const uint8_t *)l2h, len, NULL, NULL);
	memcpy(ah->value, hmac_res, 16);
	DEBUGP(DL2TP, "Tx Message with digest: %s\n", msgb_hexdump(msg));

	return 0;
}

static int l2tp_msgb_tx(struct msgb *msg)
{
	struct l2tpd_connection *l2c = msg->dst;
	struct l2tp_control_hdr *l2h;

	/* first prepend the L2TP control header */
	l2h = (struct l2tp_control_hdr *) msgb_push(msg, sizeof(*l2h));
	l2h->ver = htons(T_BIT|L_BIT|S_BIT| msgb_l2tplen(msg));
	l2h->ccid = htonl(l2c->remote.ccid);
	l2h->Ns = htons(l2c->next_tx_seq_nr++);
	l2h->Nr = htons(l2c->next_rx_seq_nr);

	/* then insert/patch the message digest AVP */
	digest_avp_update(msg);

	/* FIXME: actually transmit it */
}

static int tx_scc_rp(uint32_t ccid)
{
	struct msgb *msg = l2tp_msgb_alloc();
	const uint8_t eric_ver3_only[12] = { 0,0,0,3,  0,0,0,0, 0,0,0,0 };
	const uint8_t host_name[3] = { 'B', 'S', 'C' };
	const uint32_t router_id = 0x2342;

	msgb_avp_put_msgt(msg, VENDOR_IETF, IETF_CTRLMSG_SCCRP);
	msgb_avp_put_digest(msg);
	msgb_avp_put_u32(msg, VENDOR_IETF, AVP_IETF_AS_CTRL_CON_ID, ccid, true);
	msgb_avp_put(msg, VENDOR_ERICSSON, AVP_ERIC_PROTO_VER,
		     eric_ver3_only, sizeof(eric_ver3_only), true);
	msgb_avp_put(msg, VENDOR_IETF, AVP_IETF_HOST_NAME,
			host_name, sizeof(host_name), false);
	msgb_avp_put_u32(msg, VENDOR_IETF, AVP_IETF_ROUTER_ID,
			 router_id, false);
	msgb_avp_put_u16(msg, VENDOR_IETF, AVP_IETF_PW_CAP_LIST,
			 0x0006, true);

	return l2tp_msgb_tx(msg);
}

static int tx_tc_rq()
{
	struct msgb *msg = l2tp_msgb_alloc();
	const uint8_t tcg[] = { 0x00, 0x19, 0x01, 0x1f, 0x05, 
				0, 10, 11, 12, 62, /* SAPIs */
				10, 251, 134, 1, /* IP */
				0x00, 0x01, 0x05, 0x05, 0xb9 };

	msgb_avp_put_msgt(msg, VENDOR_ERICSSON, ERIC_CTRLMSG_TCRQ);
	msgb_avp_put_digest(msg);
	msgb_avp_put(msg, VENDOR_ERICSSON, AVP_ERIC_TRANSP_CFG,
			tcg, sizeof(tcg), true);

	return l2tp_msgb_tx(msg);
}

static int tx_altc_rq()
{
	struct msgb *msg = l2tp_msgb_alloc();
	const uint8_t tcsc[] = { 2,
				0, 0, 0,
				62, 62, 0 };

	msgb_avp_put_msgt(msg, VENDOR_ERICSSON, ERIC_CTRLMSG_ALTCRQ);
	msgb_avp_put_digest(msg);
	msgb_avp_put(msg, VENDOR_ERICSSON, AVP_ERIC_TEI_TO_SC_MAP,
			tcsc, sizeof(tcsc), true);

	return l2tp_msgb_tx(msg);
}

static int tx_tc_rp(struct l2tpd_session *ls)
{
	struct msgb *msg = l2tp_msgb_alloc();

	msgb_avp_put_msgt(msg, VENDOR_IETF, IETF_CTRLMSG_ICRP);
	msgb_avp_put_digest(msg);
	msgb_avp_put_u32(msg, VENDOR_IETF, AVP_IETF_LOC_SESS_ID,
			 ls->l_sess_id, true);
	msgb_avp_put_u32(msg, VENDOR_IETF, AVP_IETF_REM_SESS_ID,
			 ls->r_sess_id, true);
	/* Circuit type: existing; Circuit status: up */
	msgb_avp_put_u16(msg, VENDOR_IETF, AVP_IETF_CIRC_STATUS,
			 0x0001, true);
	/* Default L2 specific sublayer present */
	msgb_avp_put_u16(msg, VENDOR_IETF, AVP_IETF_L2_SPEC_SUBL,
			 0x0001, true);
	/* All incoming data packets require sequencing */
	msgb_avp_put_u16(msg, VENDOR_IETF, AVP_IETF_DATA_SEQUENCING,
			 0x0002, true);

	return l2tp_msgb_tx(msg);
}

static int tx_ack()
{
	struct msgb *msg = l2tp_msgb_alloc();

	msgb_avp_put_msgt(msg, VENDOR_IETF, IETF_CTRLMSG_ACK);
	msgb_avp_put_digest(msg);

	return l2tp_msgb_tx(msg);
}

static int tx_hello()
{
	struct msgb *msg = l2tp_msgb_alloc();

	msgb_avp_put_msgt(msg, VENDOR_IETF, IETF_CTRLMSG_HELLO);
	msgb_avp_put_digest(msg);

	return l2tp_msgb_tx(msg);
}

static int rx_scc_rq(struct msgb *msg)
{
}

static int rx_scc_cn(struct msgb *msg)
{
}

static int rx_stop_ccn(struct msgb *msg)
{
}

static int rx_ic_rq(struct msgb *msg)
{
}

static int rx_ic_cn(struct msgb *msg)
{
}

/* Receive an IETF specified control message */
static int l2tp_rcvmsg_control_ietf(struct msgb *msg, struct avp_parsed *first_ap)
{
	uint16_t msg_type;

	if (first_ap->data_len != 2) {
		LOGP(DL2TP, LOGL_ERROR, "Control Msg AVP length !=2: %u\n",
			first_ap->data_len);
		return -1;
	}

	msg_type = osmo_load16be(first_ap->data);
	switch (msg_type) {
	case IETF_CTRLMSG_SCCRQ:
		return rx_scc_rq(msg);
	case IETF_CTRLMSG_SCCCN:
		return rx_scc_cn(msg);
	case IETF_CTRLMSG_STOPCCN:
		return rx_stop_ccn(msg);
	case IETF_CTRLMSG_ICRQ:
		return rx_ic_rq(msg);
	case IETF_CTRLMSG_ICCN:
		return rx_ic_cn(msg);
	default:
		LOGP(DL2TP, LOGL_ERROR, "Unknown/Unhandled IETF Control "
			"Message Type 0x%04x\n", msg_type);
		return -1;
	}
}

static int rx_eri_tcrp(struct msgb *mgs)
{
}

static int rx_eri_altcrp(struct msgb *msg)
{
}

/* Receive an Ericsson specific control message */
static int l2tp_rcvmsg_control_ericsson(struct msgb *msg, struct avp_parsed *first_ap)
{
	uint16_t msg_type;

	if (first_ap->data_len != 2) {
		LOGP(DL2TP, LOGL_ERROR, "Control Msg AVP length !=2: %u\n",
			first_ap->data_len);
		return -1;
	}

	msg_type = osmo_load16be(first_ap->data);
	switch (msg_type) {
	case ERIC_CTRLMSG_TCRP:
		return rx_eri_tcrp(msg);
	case ERIC_CTRLMSG_ALTCRP:
		return rx_eri_altcrp(msg);
	default:
		LOGP(DL2TP, LOGL_ERROR, "Unknown/Unhandled Ericsson Control "
			"Message Type 0x%04x\n", msg_type);
		return -1;
	}
}

static int l2tp_rcvmsg_control(struct msgb *msg)
{
	struct l2tp_control_hdr *ch = msgb_l2tph(msg);
	struct avp_parsed ap;
	int rc;

	l2tp_hdr_swap(ch);

	if ((ch->ver & VER_MASK) != 3) {
		LOGP(DL2TP, LOGL_ERROR, "L2TP Version != 3\n");
		return -1;
	}

	if (ch->ver & T_BIT|L_BIT|S_BIT != T_BIT|L_BIT|S_BIT) {
		LOGP(DL2TP, LOGL_ERROR, "L2TP Bits wrong\n");
		return -1;
	}

	if (ch->ver & Z_BITS) {
		LOGP(DL2TP, LOGL_ERROR, "L2TP Z bit must not be set\n");
		return -1;
	}

	if (msgb_l2tplen(msg) < ch->length) {
		LOGP(DL2TP, LOGL_ERROR, "L2TP message length beyond msgb\n");
		return -1;
	}

	if (ch->ccid != 0) {
		LOGP(DL2TP, LOGL_ERROR, "Control Message for CCID != 0\n");
		return -1;
	}

	/* Parse the first AVP an see if it is Control Message */
	rc = msgb_avp_parse(&ap, msg, sizeof(*ch));
	if (rc < 0) {
		LOGP(DL2TP, LOGL_ERROR, "Error in parsing AVPs\n");
		return rc;
	}

	if (ap.vendor_id == VENDOR_IETF && ap.type == AVP_IETF_CTRL_MSG)
		return l2tp_rcvmsg_control_ietf(msg, &ap);
	else if (ap.vendor_id == VENDOR_ERICSSON &&
		 ap.type == AVP_ERIC_CTRL_MSG)
		return l2tp_rcvmsg_control_ericsson(msg, &ap);

}

static int l2tp_rcvmsg_data(struct msgb *msg, bool ip_transport)
{
	DEBUGP(DL2TP, "rx data: %s\n", msgb_hexdump(msg));
	return 0;
}

int l2tp_rcvmsg(struct msgb *msg, bool ip_transport)
{
	struct l2tp_control_hdr *ch;

	if (ip_transport) {
		uint32_t session = osmo_load32be(msgb_l2tph(msg));
		if (session == 0) {
			/* strip session ID and feed to control */
			msgb_pull(msg, sizeof(session));
			return l2tp_rcvmsg_control(msg);
		} else {
			return l2tp_rcvmsg_data(msg, true);
		}
	} else {
		LOGP(DL2TP, LOGL_ERROR, "UDP transport not supported (yet?)\n");
		/* FIXME */
	}
}

static int l2tp_ip_read_cb(struct osmo_fd *ofd, unsigned int what)
{
	struct msgb *msg = l2tp_msgb_alloc();
	struct sockaddr_in sin;
	socklen_t sin_len = sizeof(sin);
	int rc;

	/* actually read the message from the raw IP socket */
	msg->l2h = msg->data;
	rc = recvfrom(ofd->fd, msgb_l2tph(msg), msgb_l2tplen(msg), 0,
			(struct sockaddr *) &sin, &sin_len);
	if (rc < 0)
		return rc;
	msgb_put(msg, rc);

	/* FIXME: resolve l2tpd_connection somewhere ? */

	return l2tp_rcvmsg(msg, true);
}

struct l2tpd_instance {
	/* list of l2tpd_connection */
	struct llist_head connections;

	struct osmo_fd l2tp_ofd;

	struct {
		const char *bind_ip;
	} cfg;
};

static int l2tpd_instance_start(struct l2tpd_instance *li)
{
	int rc;

	INIT_LLIST_HEAD(&li->connections);

	rc = osmo_sock_init_ofd(&li->l2tp_ofd, AF_INET, SOCK_RAW,
				IPPROTO_L2TP, li->cfg.bind_ip, 0, 0);
	if (rc < 0)
		return rc;

	li->l2tp_ofd.when = BSC_FD_READ;
	li->l2tp_ofd.cb = l2tp_ip_read_cb;
	li->l2tp_ofd.data = li;

	return 0;
}


int main(int argc, char **argv)
{
	struct l2tpd_instance li;
	int rc;

	li.cfg.bind_ip = "0.0.0.0";
	rc = l2tpd_instance_start(&li);
	if (rc < 0)
		exit(1);

	while (1) {
		osmo_select_main(0);
	}
}
