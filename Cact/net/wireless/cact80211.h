#ifndef CACT80211_H
#define CACT80211_H

/*
 * Driver-facing API of the in-kernel 802.11 station engine (cact80211).
 *
 * The Wi-Fi driver is a dumb radio: it brings up the chip, sets the channel and
 * moves raw 802.11 MPDUs.  Everything above that — scan, auth/association, the
 * 802.3<->802.11 datapath and CCMP — lives in the engine.  A driver registers
 * one `struct cact80211_hw` from its probe and calls `cact80211_rx()` for every
 * MPDU its RX path delivers.
 *
 * The engine entry points are resolved through the kernel's ksym table.
 */

#include <stdint.h>

/* TX queue class the engine hands to the radio's tx() op.  The driver maps it
   to a hardware endpoint (management vs best-effort data) — the engine does not
   know hardware queue ids. */
#define CACT80211_Q_MGMT 0
#define CACT80211_Q_DATA 1

/* Radio configuration, programmed through the config() op whenever the engine's
   view of the medium changes (each scan channel, and association). */
struct cact80211_conf {
    uint8_t  channel;       /* primary channel (2.4 GHz: 1..13) */
    uint8_t  assoc;         /* 1 = associated: program the BSSID/interface   */
    uint8_t  bssid[6];      /* the BSS to join (when assoc)                  */
    uint16_t basic_rates;   /* AP basic-rate bitmap (bit0=1M .. bit11=54M)   */
    uint16_t erp_flags;     /* CACT80211_ERP_*                               */
};

#define CACT80211_ERP_SHORT_PREAMBLE 0x0001
#define CACT80211_ERP_CTS_PROT       0x0002
#define CACT80211_ERP_SHORT_SLOT     0x0004

struct cact80211_ops {
    /* Transmit one raw 802.11 MPDU (no FCS) on the given queue class; returns
       0 on success or a negative errno. */
    int  (*tx)(void *priv, const uint8_t *mpdu, int len, uint8_t qid);

    /* Apply the radio configuration above. */
    void (*config)(void *priv, const struct cact80211_conf *conf);

    /* Drive one receive poll; call cact80211_rx() for each delivered frame.
       Returns frames delivered, or <= 0 when idle. */
    int  (*rx_poll)(void *priv);
};

struct cact80211_hw {
    const struct cact80211_ops *ops;
    void        *priv;      /* the driver's per-device context                 */
    uint8_t      mac[6];    /* station address                                 */
    const char  *name;      /* interface name ("wlan0")                        */
};

/* Register a radio with the engine: brings up wlan0 (netdev + /dev/wlan0).
   Returns 0 on success, a negative errno otherwise. */
int  cact80211_register(struct cact80211_hw *hw);

/* Tear the radio down (netdev + control node + engine state). */
void cact80211_unregister(struct cact80211_hw *hw);

/* Hand one received 802.11 MPDU to the engine (from the driver's rx_poll). */
void cact80211_rx(struct cact80211_hw *hw, const uint8_t *frame, int len);

#endif /* CACT80211_H */
