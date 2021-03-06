// SPDX-License-Identifier: BSD-2-Clause
//
// Copyright (c) 2016-2017, NetApp, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#ifdef __linux__
#include <byteswap.h>
#endif

#include <quant/quant.h>
#include <warpcore/warpcore.h>

#include "conn.h"
#include "diet.h"
#include "frame.h"
#include "marshall.h"
#include "pkt.h"
#include "quic.h"
#include "recovery.h"
#include "stream.h"
#include "tls.h"


/// Packet number lengths for different short-header packet types
static const uint8_t pkt_nr_lens[] = {0xFF, 1, 2, 4};


uint16_t pkt_hdr_len(const uint8_t * const buf, const uint16_t len)
{
    const uint8_t flags = pkt_flags(buf);
    uint16_t pos = 0;
    if (is_set(F_LONG_HDR, flags))
        pos = 17;
    else {
        const uint8_t type = pkt_type(flags);
        if (type < 1 || type > 3) {
            warn(ERR, "illegal pkt type %u", type);
            return UINT16_MAX;
        }
        pos = 1 + (is_set(F_SH_CID, flags) ? 8 : 0) + pkt_nr_lens[type];
    }
    ensure(pos <= len, "payload position %u after end of packet %u", pos, len);
    return pos;
}


uint64_t pkt_cid(const uint8_t * const buf, const uint16_t len)
{
    const uint8_t flags = pkt_flags(buf);
    uint64_t cid = 0;
    if (is_set(F_LONG_HDR, flags) || is_set(F_SH_CID, flags)) {
        uint16_t i = 1;
        dec(cid, buf, len, i, 0, FMT_CID);
    } else
        die("no connection ID in header");
    return cid;
}


uint64_t
pkt_nr(const uint8_t * const buf, const uint16_t len, struct q_conn * const c)
{
    const uint64_t next = diet_max(&c->recv) + 1;
    const uint8_t flags = pkt_flags(buf);
    const uint8_t nr_len =
        is_set(F_LONG_HDR, flags) ? 4 : pkt_nr_lens[pkt_type(flags)];
    ensure(nr_len <= 4, "illegal packet number len %u", nr_len);

    uint16_t i = is_set(F_LONG_HDR, flags) || is_set(F_SH_CID, flags) ? 9 : 1;
    uint64_t nr = next;
    dec(nr, buf, len, i, nr_len, FMT_PNR);

    // const uint64_t mask[] = {0x0, 0x100UL, 0x10000UL, 0x0, 0x100000000UL};
    const uint64_t alt = nr + (UINT64_C(1) << (nr_len * 8));
    const uint64_t d1 = next >= nr ? next - nr : nr - next;
    const uint64_t d2 = next >= alt ? next - alt : alt - next;

    return d1 < d2 ? nr : alt;
}


uint32_t pkt_vers(const uint8_t * const buf, const uint16_t len)
{
    ensure(is_set(F_LONG_HDR, pkt_flags(buf)), "short header");
    uint32_t vers = 0;
    uint16_t i = 13;
    dec(vers, buf, len, i, 0, "0x%08x");
    return vers;
}


static const uint8_t enc_pkt_nr_len[] = {0xFF, 1, 2, 0xFF, 3};


static uint8_t __attribute__((nonnull))
needed_pkt_nr_len(struct q_conn * const c, const uint64_t n)
{
    const uint64_t d = (n - c->rec.lg_acked) * 2;
    if (d < UINT8_MAX)
        return 1;
    if (d < UINT16_MAX)
        return 2;
    return 4;
}


bool enc_pkt(struct q_stream * const s,
             const bool rtx,
             struct w_iov * const v,
             struct w_iov_sq * const q)
{
    // prepend the header by adjusting the buffer offset
    adj_iov_to_start(v);

    struct q_conn * const c = s->c;

#ifndef NDEBUG
    if (rtx) {
        const uint64_t prev_nr = meta(v).nr;
        warn(INF, "enc RTX " FMT_PNR " as " FMT_PNR " in idx %u", prev_nr,
             c->state == CONN_STAT_VERS_REJ ? diet_max(&c->recv)
                                            : c->rec.lg_sent + 1,
             w_iov_idx(v));
    }
#endif

    meta(v).nr =
        c->state == CONN_STAT_VERS_REJ ? diet_max(&c->recv) : ++c->rec.lg_sent;
    // TODO: increase by random offset

    const uint8_t pkt_nr_len = needed_pkt_nr_len(c, meta(v).nr);
    uint8_t flags = 0;
    switch (c->state) {
    case CONN_STAT_VERS_SENT:
    case CONN_STAT_RETRY:
        flags |= F_LONG_HDR | F_LH_CLNT_INIT;
        break;
    case CONN_STAT_VERS_REJ:
        flags |= F_LONG_HDR | F_LH_TYPE_VNEG;
        break;
    case CONN_STAT_IDLE:
    case CONN_STAT_VERS_OK:
        flags |= F_LONG_HDR | (c->is_clnt ? F_LH_CLNT_CTXT : F_LH_SERV_CTXT);
        break;
    case CONN_STAT_ESTB:
    case CONN_STAT_CLSD:
        if (!c->omit_cid)
            flags |= F_SH_CID;
        flags |= enc_pkt_nr_len[pkt_nr_len];
        break;
    default:
        die("unknown conn state %u", c->state);
    }

    if (rtx && flags != pkt_flags(v->buf)) {
        warn(NTE,
             "RTX of 0x%02x-type pkt " FMT_PNR
             " prevented; new type would be 0x%02x",
             pkt_flags(v->buf), meta(v).nr, flags);
        adj_iov_to_data(v);
        return false;
    }

    uint16_t i = 0;
    enc(v->buf, v->len, i, &flags, 0, "0x%02x");

    if (is_set(F_LONG_HDR, flags) || is_set(F_SH_CID, flags))
        enc(v->buf, v->len, i, &c->id, 0, FMT_CID);

    if (is_set(F_LONG_HDR, flags)) {
        enc(v->buf, v->len, i, &meta(v).nr, sizeof(uint32_t), "%u");
        enc(v->buf, v->len, i, &c->vers, 0, "0x%08x");
        if (c->state == CONN_STAT_VERS_REJ) {
            warn(INF, "sending version negotiation server response");
            for (uint8_t j = 0; j < ok_vers_len; j++)
                if (!is_force_neg_vers(ok_vers[j]))
                    enc(v->buf, v->len, i, &ok_vers[j], 0, "0x%08x");
            v->len = i;
            // don't remember the failed client initial
            diet_remove(&c->recv, meta(v).nr);
        }
    } else
        enc(v->buf, v->len, i, &meta(v).nr, pkt_nr_len, FMT_PNR);

    const uint16_t hdr_len = i;

    if (c->state != CONN_STAT_VERS_REJ && c->state != CONN_STAT_RETRY &&
        !splay_empty(&c->recv)) {
        meta(v).ack_header_pos = i;
        i += enc_ack_frame(c, v, i);
    } else
        meta(v).ack_header_pos = 0;


    // TODO: Unclear whether this is the best way to send this in the long run.
    // warn(NTE, "str %u out_off %u/%u", s->id, s->out_off, s->out_off_max);
    if (s->out_off_max && s->out_off + MAX_PKT_LEN > s->out_off_max) {
        // if we have less than one full packet's worth of window, block
        s->blocked = true;
        adj_iov_to_data(v);
        return false;
    }
    if (s->out_off_max && s->out_off + 2 * MAX_PKT_LEN > s->out_off_max)
        // if we have less than two full packets' worth of window, notify
        i += enc_stream_blocked_frame(s, v, i);

    // TODO: Unclear whether this is the best way to send this in the long run.
    // warn(NTE, "str %u in_off %u/%u", s->id, s->in_off, s->in_off_max);
    if (s->open_win || s->in_off + MAX_PKT_LEN > s->in_off_max) {
        // increase receive window
        s->in_off_max += 0x1000;
        i += enc_max_stream_data_frame(s, v, i);
        s->open_win = false;
    }

    // TODO: need to RTX most recent MAX_STREAM_DATA and MAX_DATA on RTX

    if (c->state == CONN_STAT_CLSD) {
        i += enc_close_frame(v, i, FRAM_TYPE_CONN_CLSE, CONN_CLSE_ERR_NO_ERROR,
                             "QUANT SAYS GOOD-BYE");
        maybe_api_return(q_close, c);
    }

    if (rtx) {
        ensure(is_rtxable(&meta(v)), "is rtxable");

        // this is a RTX, pad out until beginning of stream header
        enc_padding_frame(v, i, meta(v).stream_header_pos - i);
        i = meta(v).stream_data_end;

    } else {
        if (v->len > Q_OFFSET || s->state == STRM_STAT_HCLO ||
            s->state == STRM_STAT_CLSD) {
            // this is a fresh data or pure FIN packet
            // add a stream frame header, after padding out rest of Q_OFFSET
            enc_padding_frame(v, i, Q_OFFSET - i);
            i = enc_stream_frame(s, v);
        }
    }

    if (c->state == CONN_STAT_VERS_SENT)
        i += enc_padding_frame(v, i, MIN_INI_LEN - i - AEAD_LEN);
    v->len = i;

    // alloc a new buffer to encrypt/sign into for TX
    struct w_iov * const x = q_alloc_iov(w_engine(c->sock), MAX_PKT_LEN, 0);
    x->ip = v->ip;
    x->port = v->port;
    x->flags = v->flags;

    if (c->state == CONN_STAT_VERS_REJ) {
        memcpy(x->buf, v->buf, v->len); // copy data
        x->len = v->len;
    } else
        x->len = enc_aead(c, v, x, hdr_len);

    sq_insert_tail(q, x, next);
    meta(v).tx_len = x->len;

    warn(NTE,
         "tx pkt " FMT_PNR " (%" PRIx64
         "), len %u+%u, type 0x%02x on %s conn " FMT_CID,
         meta(v).nr, meta(v).nr, v->len, meta(v).tx_len - v->len,
         pkt_flags(v->buf), conn_type(c), c->id);

    if (c->state == CONN_STAT_VERS_SENT)
        // adjust v->len to end of stream data (excl. padding)
        v->len = meta(v).stream_data_end;

    adj_iov_to_data(v);
    return true;
}
