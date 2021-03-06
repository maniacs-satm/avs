/*
* Wire
* Copyright (C) 2016 Wire Swiss GmbH
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*/


/*
 * Mediaflow
 */

struct mediaflow;
struct zapi_candidate;
struct aucodec_stats;
struct rtp_stats;

enum media_pt {
	MEDIA_PT_DYNAMIC_START =  96,
	MEDIA_PT_DYNAMIC_END   = 127,

	/* custom range of Payload-types for audio/video,
	   to properly support Bundle multiplexing */
	MEDIA_PT_AUDIO_START =  96,
	MEDIA_PT_AUDIO_END   =  99,
	MEDIA_PT_VIDEO_START = 100,
	MEDIA_PT_VIDEO_END   = 110,
};

enum mediaflow_nat {
	MEDIAFLOW_NAT_NONE = 0,
	MEDIAFLOW_TRICKLEICE_DUALSTACK,
	MEDIAFLOW_ICELITE,
	MEDIAFLOW_TURN,
};

enum media_crypto {
	CRYPTO_NONE      = 0,
	CRYPTO_DTLS_SRTP = 1<<0,
	CRYPTO_SDESC     = 1<<1
};

/* only valid for DTLS-SRTP */
enum media_setup {
	SETUP_ACTPASS,
	SETUP_ACTIVE,
	SETUP_PASSIVE
};

enum media_type {
	MEDIA_AUDIO = 0,
	MEDIA_VIDEO = 1,
	MEDIA_VIDEO_RTX = 2,
	/* sep */
	MEDIA_NUM = 3,
};

/*
 * Mediaflow statistics in [ms]
 *
 * -2  error
 * -1  init
 */
struct mediaflow_stats {
	int32_t turn_alloc;
	int32_t nat_estab;
	int32_t dtls_estab;

	unsigned dtls_pkt_sent;
	unsigned dtls_pkt_recv;
};


typedef void (mediaflow_localcand_h)(const struct zapi_candidate *candv,
				     size_t candc, void *arg);
typedef void (mediaflow_estab_h)(const char *crypto, const char *codec,
				 const char *type, const struct sa *sa,
				 void *arg);
typedef void (mediaflow_audio_h)(const int16_t *sampv, size_t sampc,
				 void *arg);
typedef void (mediaflow_rtp_h)(const struct sa *src,
			       const struct rtp_header *hdr, struct mbuf *mb,
			       void *arg);
typedef void (mediaflow_rtcp_h)(struct rtp_sock *rtp,
				struct rtcp_msg *msg, void *arg);
typedef void (mediaflow_close_h)(int err, void *arg);

typedef void (mediaflow_rtp_state_h)(bool started, bool video_started,
				     void *arg);

typedef void (mediaflow_gather_h)(void *arg);

int mediaflow_alloc(struct mediaflow **mfp, struct tls *dtls,
		    const struct list *aucodecl,
		    const struct sa *laddr,
		    enum mediaflow_nat nat,
		    enum media_crypto cryptos,
		    bool external_rtp,
		    mediaflow_localcand_h *lcandh,
		    mediaflow_estab_h *estabh,
		    mediaflow_close_h *closeh,
		    void *arg);

int mediaflow_set_setup(struct mediaflow *mf, enum media_setup setup);
enum media_setup mediaflow_local_setup(const struct mediaflow *mf);

void mediaflow_set_earlydtls(struct mediaflow *mf, bool enabled);
bool mediaflow_early_dtls_supported(const struct mediaflow *mf);

void mediaflow_set_rtp_handler(struct mediaflow *mf,
			       uint32_t audio_srate, uint8_t audio_ch,
			       mediaflow_audio_h *audioh,
			       mediaflow_rtp_h *rtph,
			       mediaflow_rtcp_h *rtcph);
int mediaflow_add_video(struct mediaflow *mf, struct list *vidcodecl);
void mediaflow_set_gather_handler(struct mediaflow *mf,
				  mediaflow_gather_h *gatherh);

int mediaflow_start_ice(struct mediaflow *mf);

int mediaflow_start_media(struct mediaflow *mf);
void mediaflow_stop_media(struct mediaflow *mf);
void mediaflow_reset_media(struct mediaflow *mf);


int mediaflow_set_video_send_active(struct mediaflow *mf, bool video_active);

void mediaflow_set_tag(struct mediaflow *mf, const char *tag);
int mediaflow_add_local_host_candidate(struct mediaflow *mf,
				       const char *ifname,
				       const struct sa *addr);
int mediaflow_add_rcand(struct mediaflow *mf, const char *rcand,
			const char *mid, int idx);
int mediaflow_generate_offer(struct mediaflow *mf, char *sdp, size_t sz);
int mediaflow_generate_answer(struct mediaflow *mf, char *sdp, size_t sz);
int mediaflow_handle_offer(struct mediaflow *mf, const char *sdp);
int mediaflow_handle_answer(struct mediaflow *mf, const char *sdp);
int mediaflow_offeranswer(struct mediaflow *mf,
			  char *answer, size_t answer_sz,
			  const char *offer);
void mediaflow_sdpstate_reset(struct mediaflow *mf);
int mediaflow_send_audio(struct mediaflow *mf,
			 const int16_t *sampv, size_t sampc);
int mediaflow_send_rtp(struct mediaflow *mf, const struct rtp_header *hdr,
		       const uint8_t *pld, size_t pldlen);
int mediaflow_send_raw_rtp(struct mediaflow *mf,
			   const uint8_t *buf, size_t len);
int mediaflow_send_raw_rtcp(struct mediaflow *mf,
			    const uint8_t *buf, size_t len);
bool mediaflow_is_ready(const struct mediaflow *mf);
int mediaflow_get_rtcpstats(struct mediaflow *mf, struct rtcp_stats *stats);
int mediaflow_gather_stun(struct mediaflow *mf, const struct sa *stun_srv);
int mediaflow_gather_turn(struct mediaflow *mf, const struct sa *turn_srv,
			  const char *username, const char *password);
int mediaflow_gather_turn_tcp(struct mediaflow *mf, const struct sa *turn_srv,
			      const char *username, const char *password,
			      bool secure);
uint16_t mediaflow_lport(const struct mediaflow *mf);
size_t mediaflow_remote_cand_count(const struct mediaflow *mf);
int mediaflow_summary(struct re_printf *pf, const struct mediaflow *mf);
int mediaflow_rtp_summary(struct re_printf *pf, const struct mediaflow *mf);
enum media_crypto mediaflow_crypto(const struct mediaflow *mf);

struct auenc_state *mediaflow_encoder(const struct mediaflow *mf);
struct audec_state *mediaflow_decoder(const struct mediaflow *mf);
int mediaflow_debug(struct re_printf *pf, const struct mediaflow *mf);

void mediaflow_set_rtpstate_handler(struct mediaflow *mf,
				      mediaflow_rtp_state_h *rtpstateh);
const char *mediaflow_peer_software(const struct mediaflow *mf);

void mediaflow_rtp_start_send(struct mediaflow *mf);
bool mediaflow_has_video(const struct mediaflow *mf);
bool mediaflow_is_sending_video(struct mediaflow *mf);
int  mediaflow_video_debug(struct re_printf *pf, const struct mediaflow *mf);

struct videnc_state *mediaflow_video_encoder(const struct mediaflow *mf);
struct viddec_state *mediaflow_video_decoder(const struct mediaflow *mf);

const struct tls_conn *mediaflow_dtls_connection(const struct mediaflow *mf);

bool mediaflow_is_started(const struct mediaflow *mf);

bool mediaflow_got_sdp(const struct mediaflow *mf);
bool mediaflow_sdp_is_complete(const struct mediaflow *mf);
bool mediaflow_is_gathered(const struct mediaflow *mf);

uint32_t mediaflow_get_local_ssrc(struct mediaflow *mf, enum media_type type);
int mediaflow_get_remote_ssrc(const struct mediaflow *mf,
			      enum media_type type, uint32_t *ssrcp);

bool mediaflow_dtls_ready(struct mediaflow *mf);
bool mediaflow_ice_ready(struct mediaflow *mf);
bool mediaflow_is_rtpstarted(const struct mediaflow *mf);
int  mediaflow_cryptos_print(struct re_printf *pf, enum media_crypto cryptos);
const char *mediaflow_nat_name(enum mediaflow_nat nat);
enum mediaflow_nat mediaflow_nat_resolve(const char *name);


struct rtp_stats* mediaflow_rcv_audio_rtp_stats(struct mediaflow *mf);
struct rtp_stats* mediaflow_snd_audio_rtp_stats(struct mediaflow *mf);
struct rtp_stats* mediaflow_rcv_video_rtp_stats(struct mediaflow *mf);
struct rtp_stats* mediaflow_snd_video_rtp_stats(struct mediaflow *mf);
struct aucodec_stats* mediaflow_codec_stats(struct mediaflow *mf);

int mediaflow_hold_media(struct mediaflow *mf, bool hold);

const struct mediaflow_stats *mediaflow_stats_get(const struct mediaflow *mf);

void mediaflow_set_local_eoc(struct mediaflow *mf);
bool mediaflow_have_eoc(const struct mediaflow *mf);
void mediaflow_enable_privacy(struct mediaflow *mf, bool enabled);

const char *mediaflow_lcand_name(const struct mediaflow *mf);
const char *mediaflow_rcand_name(const struct mediaflow *mf);
bool mediaflow_dtls_peer_isset(const struct mediaflow *mf);
