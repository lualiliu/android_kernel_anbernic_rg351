/* Copyright (c) 2008 -2014 Espressif System.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *
 *     MAC80211 support module
 */

#include <linux/etherdevice.h>
#include <linux/workqueue.h>
#include <linux/nl80211.h>
#include <linux/ieee80211.h>
#include <linux/slab.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>
#include <linux/version.h>
#include <net/regulatory.h>
/* for support scan in p2p concurrent */
#include <../net/mac80211/ieee80211_i.h>
#include "esp_pub.h"
#include "esp_sip.h"
#include "esp_ctrl.h"
#include "esp_sif.h"
#include "esp_debug.h"
#include "esp_wl.h"
#include "esp_utils.h"
#include <linux/rfkill-wlan.h>
#include "esp_mac80211.h"
#define ESP_IEEE80211_DBG esp_dbg

#define GET_NEXT_SEQ(seq) (((seq) +1) & 0x0fff)

extern void reset_signal_count(void);


static void beacon_tim_init(void);
static u8 beacon_tim_save(u8 this_tim);
static bool beacon_tim_alter(struct sk_buff *beacon);

#ifdef P2P_CONCURRENT
static u8 esp_mac_addr[ETH_ALEN * 2];
#endif
static u8 getaddr_index(u8 * addr, struct esp_pub *epub);

/*Handler that 802.11 module calls for each transmitted frame.
skb contains the buffer starting from the IEEE 802.11 header.
The low-level driver should send the frame out based on
configuration in the TX control data. This handler should,
preferably, never fail and stop queues appropriately.
Must be atomic.*/
static void esp_op_tx(struct ieee80211_hw *hw,
		      struct ieee80211_tx_control *control, struct sk_buff *skb)
{
	struct esp_pub *epub = (struct esp_pub *)hw->priv;

	ESP_IEEE80211_DBG(ESP_DBG_LOG, "%s enter\n", __func__);
	if (!mod_support_no_txampdu() &&
                	cfg80211_get_chandef_type(&epub->hw->conf.chandef) != NL80211_CHAN_NO_HT

	   ) {
		struct ieee80211_tx_info * tx_info = IEEE80211_SKB_CB(skb);
		struct ieee80211_hdr * wh = (struct ieee80211_hdr *)skb->data;
		if(ieee80211_is_data_qos(wh->frame_control)) {
			if(!(tx_info->flags & IEEE80211_TX_CTL_AMPDU)) {
				u8 tidno = ieee80211_get_qos_ctl(wh)[0] & IEEE80211_QOS_CTL_TID_MASK;
				struct ieee80211_sta *sta = control->sta;
				struct esp_node * node = (struct esp_node *)sta->drv_priv;
				if(sta->ht_cap.ht_supported)

				{
					struct esp_tx_tid *tid = &node->tid[tidno];
					//record ssn
					spin_lock_bh(&epub->tx_ampdu_lock);
					tid->ssn = GET_NEXT_SEQ(le16_to_cpu(wh->seq_ctrl)>>4);
					ESP_IEEE80211_DBG(ESP_DBG_TRACE, "tidno:%u,ssn:%u\n", tidno, tid->ssn);
					spin_unlock_bh(&epub->tx_ampdu_lock);
				}
			} else {
				ESP_IEEE80211_DBG(ESP_DBG_TRACE, "tx ampdu pkt, sn:%u, %u\n", le16_to_cpu(wh->seq_ctrl)>>4, skb->len);
			}
		}
	}

#ifdef GEN_ERR_CHECKSUM
	esp_gen_err_checksum(skb);
#endif

	sip_tx_data_pkt_enqueue(epub, skb);

}

/*
   Called before the first netdevice attached to the hardware
   2934  *      is enabled. This should turn on the hardware and must turn on
   2935  *      frame reception (for possibly enabled monitor interfaces.)
   2936  *      Returns negative error codes, these may be seen in userspace,
   2937  *      or zero.
   2938  *      When the device is started it should not have a MAC address
   2939  *      to avoid acknowledging frames before a non-monitor device
   2940  *      is added.
   2941  *      Must be implemented and can sleep.
*/
static int esp_op_start(struct ieee80211_hw *hw)
{
	struct esp_pub *epub;

	ESP_IEEE80211_DBG(ESP_DBG_OP, "%s\n", __func__);

	if (!hw || !hw->priv) {
		ESP_IEEE80211_DBG(ESP_DBG_ERROR, "%s no hw!\n", __func__);
		return -EINVAL;
	}

	epub = (struct esp_pub *)hw->priv;

	/*add rfkill poll function*/

	atomic_set(&epub->wl.off, 0);
	wiphy_rfkill_start_polling(hw->wiphy);

	return 0;
}

/*
Called after last netdevice attached to the hardware
2944  *      is disabled. This should turn off the hardware (at least
2945  *      it must turn off frame reception.)
2946  *      May be called right after add_interface if that rejects
2947  *      an interface. If you added any work onto the mac80211 workqueue
2948  *      you should ensure to cancel it on this callback.
2949  *      Must be implemented and can sleep.
*/
static void esp_op_stop(struct ieee80211_hw *hw)
{
	struct esp_pub *epub;

	ESP_IEEE80211_DBG(ESP_DBG_OP, "%s\n", __func__);

	if (!hw || !hw->priv) {
		ESP_IEEE80211_DBG(ESP_DBG_ERROR, "%s no hw!\n", __func__);
		return;
	}

	epub = (struct esp_pub *)hw->priv;
	atomic_set(&epub->wl.off, 1);

#ifdef HOST_RESET_BUG
	mdelay(200);
#endif

	if (epub->wl.scan_req) {
		hw_scan_done(epub, true);
		epub->wl.scan_req=NULL;
		//msleep(2);
	}
	/* FIXME: does this 'turn off frame reception'? */
	wiphy_rfkill_stop_polling(hw->wiphy);
	/* FIXME: flush queues? */	
}

static int esp_set_svif_mode(struct sip_cmd_setvif *svif,
			     enum nl80211_iftype type, bool p2p)
{
	switch (type) {
	case NL80211_IFTYPE_STATION:
		svif->op_mode = 0;
		svif->is_p2p = p2p;
		break;

	case NL80211_IFTYPE_AP:
		svif->op_mode = 1;
		svif->is_p2p = p2p;
		break;

	case NL80211_IFTYPE_P2P_CLIENT:
		svif->op_mode = 0;
		svif->is_p2p = 1;
		break;

	case NL80211_IFTYPE_P2P_GO:
		svif->op_mode = 1;
		svif->is_p2p = 1;
		break;

	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

/*
Called when a netdevice attached to the hardware is
2973  *      enabled. Because it is not called for monitor mode devices, @start
2974  *      and @stop must be implemented.
2975  *      The driver should perform any initialization it needs before
2976  *      the device can be enabled. The initial configuration for the
2977  *      interface is given in the conf parameter.
2978  *      The callback may refuse to add an interface by returning a
2979  *      negative error code (which will be seen in userspace.)
2980  *      Must be implemented and can sleep.
   */
static int esp_op_add_interface(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif)
{
	struct esp_pub *epub = (struct esp_pub *)hw->priv;
	struct esp_vif *evif = (struct esp_vif *)vif->drv_priv;
	struct sip_cmd_setvif svif;
	int ret;
	
	ESP_IEEE80211_DBG(ESP_DBG_OP, "%s enter: type %d, addr %pM\n", __func__, vif->type, vif->addr);

	memcpy(svif.mac, vif->addr, ETH_ALEN);
	svif.index = getaddr_index(vif->addr, epub);
	evif->index = svif.index;
	evif->epub = epub;
	/* FIXME: why a need for evif? */
	epub->vif = vif;
	svif.set = 1;

	if (svif.index == ESP_PUB_MAX_VIF) {
		ESP_IEEE80211_DBG(ESP_DBG_ERROR, "%s only support MAX %d interface\n", __func__, ESP_PUB_MAX_VIF);
		return -EOPNOTSUPP;
	}	
	
	if (BIT(svif.index) & epub->vif_slot) {
		ESP_IEEE80211_DBG(ESP_DBG_ERROR, "%s interface %d already used\n", __func__, svif.index);
		return -EOPNOTSUPP;
	}
	
	epub->vif_slot |= BIT(svif.index);

	ret = esp_set_svif_mode(&svif, vif->type, false);
	if (ret < 0) {
		dev_err(epub->dev, "no support for interface type %d\n",
			vif->type);
		return ret;
	}



	sip_cmd(epub, SIP_CMD_SETVIF, (u8 *)&svif, sizeof(svif));

	return 0;
}

/*
Called when a netdevice changes type. This callback
2983  *      is optional, but only if it is supported can interface types be
2984  *      switched while the interface is UP. The callback may sleep.
2985  *      Note that while an interface is being switched, it will not be
2986  *      found by the interface iteration callbacks.
   */
static int esp_op_change_interface(struct ieee80211_hw *hw,
                                   struct ieee80211_vif *vif,
                                   enum nl80211_iftype new_type, bool p2p)
{
	struct esp_pub *epub = (struct esp_pub *)hw->priv;
	struct esp_vif *evif = (struct esp_vif *)vif->drv_priv;
	struct sip_cmd_setvif svif;
	int ret;

	ESP_IEEE80211_DBG(ESP_DBG_OP, "%s enter,change to if:%d \n", __func__, new_type);
	memcpy(svif.mac, vif->addr, ETH_ALEN);
	svif.index = evif->index;
	svif.set = 2;

	ret = esp_set_svif_mode(&svif, new_type, p2p);
	if (ret < 0)
		return ret;
	
	sip_cmd(epub, SIP_CMD_SETVIF, (u8 *)&svif, sizeof(svif));

        return 0;
}

/*
   Notifies a driver that an interface is going down.
   2989  *      The @stop callback is called after this if it is the last interface
   2990  *      and no monitor interfaces are present.
   2991  *      When all interfaces are removed, the MAC address in the hardware
   2992  *      must be cleared so the device no longer acknowledges packets,
   2993  *      the mac_addr member of the conf structure is, however, set to the
   2994  *      MAC address of the device going away.
   2995  *      Hence, this callback must be implemented. It can sleep.
   */
static void esp_op_remove_interface(struct ieee80211_hw *hw,
                                    struct ieee80211_vif *vif)
{
	struct esp_pub *epub = (struct esp_pub *)hw->priv;
	struct esp_vif *evif = (struct esp_vif *)vif->drv_priv;
	struct sip_cmd_setvif svif = {0};

	ESP_IEEE80211_DBG(ESP_DBG_OP, "%s enter, vif addr %pM, beacon enable %x\n", __func__, vif->addr, vif->bss_conf.enable_beacon);

	svif.index = evif->index;
	epub->vif_slot &= ~BIT(svif.index);

	if(evif->ap_up){
		evif->beacon_interval = 0;
		del_timer_sync(&evif->beacon_timer);
		evif->ap_up = false;
	}
	epub->vif = NULL;
	evif->epub = NULL;

	sip_cmd(epub, SIP_CMD_SETVIF, (u8 *)&svif, sizeof(svif));
	/* RODO: clean up tx/rx queue */
}

#define BEACON_TIM_SAVE_MAX 12
u8 beacon_tim_saved[BEACON_TIM_SAVE_MAX];
int beacon_tim_count;
spinlock_t tim_lock;
static void beacon_tim_init(void)
{
	memset(beacon_tim_saved, 0, BEACON_TIM_SAVE_MAX);
	beacon_tim_count = 0;
	spin_lock_init(&tim_lock);
}

static u8 beacon_tim_save(u8 this_tim)
{
	u8 all_tim = 0;
	int i;

	spin_lock(&tim_lock);

	beacon_tim_saved[beacon_tim_count] = this_tim;

	if(++beacon_tim_count >= BEACON_TIM_SAVE_MAX)
		beacon_tim_count = 0;

	for(i = 0; i < BEACON_TIM_SAVE_MAX; i++)
		all_tim |= beacon_tim_saved[i];
		
	spin_unlock(&tim_lock);

	return all_tim;
}

static bool beacon_tim_alter(struct sk_buff *beacon)
{
        u8 *p, *tim_end;
	u8 tim_count;
        int len;
        int remain_len;
        struct ieee80211_mgmt * mgmt;

        if (!beacon)
                return false;

        mgmt = (struct ieee80211_mgmt *)((u8 *)beacon->data);

        remain_len = beacon->len - ((u8 *)mgmt->u.beacon.variable -
        			     (u8 *)mgmt + 12);
        p = mgmt->u.beacon.variable;

        while (remain_len > 0) {
                len = *(++p);       
                
                if (*p == WLAN_EID_TIM) {       // tim field
                       tim_end = p + len;
			tim_count = *(++p);
			p += 2;
			//multicast
			if(tim_count == 0)
			    *p |= 0x1;

			if (!(*p & 0xfe) && tim_end >= p+1){// we only support 8 sta in this case
                        	p++;
				*p = beacon_tim_save(*p);
			}
                        return tim_count == 0;
                } else {
                	p += (len + 1);
		}
                remain_len -= (2 + len);
        }

        return false;
}

unsigned long init_jiffies;
unsigned long cycle_beacon_count;
struct ieee80211_vif *_gvif = NULL;
static void drv_handle_beacon(struct timer_list *list)
{

	struct ieee80211_vif *vif = (struct ieee80211_vif *) _gvif;
	struct esp_vif *evif = (struct esp_vif *)vif->drv_priv;
	struct sk_buff *beacon;
	struct sk_buff *skb;
	static int dbgcnt = 0;
	bool tim_reach = false;

	if (!evif->epub)
		return;

	mdelay(2400 * (cycle_beacon_count % 25) % 10000 /1000);
	
	beacon = ieee80211_beacon_get(evif->epub->hw, vif);

	tim_reach = beacon_tim_alter(beacon);

	if (beacon && !(dbgcnt++ % 600)) {
		ESP_IEEE80211_DBG(ESP_SHOW, " beacon length:%d,fc:0x%x\n", beacon->len,
			((struct ieee80211_mgmt *)(beacon->data))->frame_control);
	}

	if(beacon)
		sip_tx_data_pkt_enqueue(evif->epub, beacon);

	if(cycle_beacon_count++ == 100){
		init_jiffies = jiffies;
		cycle_beacon_count -= 100;
	}

	mod_timer(&evif->beacon_timer, init_jiffies +
	 	  msecs_to_jiffies(cycle_beacon_count * vif->bss_conf.beacon_int*1024/1000));

	//FIXME:the packets must be sent at home channel
	//send buffer mcast frames
	if(tim_reach){
		skb = ieee80211_get_buffered_bc(evif->epub->hw, vif);
		while (skb) {
			sip_tx_data_pkt_enqueue(evif->epub, skb);
			skb = ieee80211_get_buffered_bc(evif->epub->hw, vif);
		}
	}
}

static void init_beacon_timer(struct ieee80211_vif *vif)
{
	struct esp_vif *evif = (struct esp_vif *)vif->drv_priv;

	ESP_IEEE80211_DBG(ESP_DBG_OP, " %s enter: beacon interval %x\n", __func__, evif->beacon_interval);

	beacon_tim_init();

	_gvif = vif;
	timer_setup(&evif->beacon_timer, drv_handle_beacon, 0);
	evif->beacon_timer.expires = init_jiffies +
		msecs_to_jiffies(cycle_beacon_count * vif->bss_conf.beacon_int * 
				1024/1000);
	add_timer(&evif->beacon_timer);
}

/*
   Handler for configuration requests. IEEE 802.11 code calls this
   2998  *      function to change hardware configuration, e.g., channel.
   2999  *      This function should never fail but returns a negative error code
   3000  *      if it does. The callback can sleep.
*/
static int esp_op_config(struct ieee80211_hw *hw, u32 changed)
{
	struct esp_pub *epub = (struct esp_pub *)hw->priv;

        ESP_IEEE80211_DBG(ESP_DBG_TRACE, "%s enter 0x%08x\n", __func__, changed);

        if (changed & (IEEE80211_CONF_CHANGE_CHANNEL |
        		IEEE80211_CONF_CHANGE_IDLE))        		
                sip_send_config(epub, &hw->conf);

    return 0;
}

/*
Handler for configuration requests related to BSS
3003  *      parameters that may vary during BSS's lifespan, and may affect low
3004  *      level driver (e.g. assoc/disassoc status, erp parameters).
3005  *      This function should not be used if no BSS has been set, unless
3006  *      for association indication. The @changed parameter indicates which
3007  *      of the bss parameters has changed when a call is made. The callback
3008  *      can sleep.
   */
static void esp_op_bss_info_changed(struct ieee80211_hw *hw,
                                    struct ieee80211_vif *vif,
                                    struct ieee80211_bss_conf *info,
                                    u32 changed)
{
        struct esp_pub *epub = (struct esp_pub *)hw->priv;
        struct esp_vif *evif = (struct esp_vif *)vif->drv_priv;
	u8 *bssid = (u8 *)info->bssid;
	bool assoc = info->assoc;


	if (vif->type == NL80211_IFTYPE_STATION) {
		if (changed & BSS_CHANGED_BSSID ||
		    ((changed & BSS_CHANGED_ASSOC) && assoc)) {
			evif->beacon_interval = info->aid;
			memcpy(epub->wl.bssid, bssid, ETH_ALEN);
			sip_send_bss_info_update(epub, evif, bssid, assoc);
		} else if ((changed & BSS_CHANGED_ASSOC) && !assoc) {
			evif->beacon_interval = 0;
			memset(epub->wl.bssid, 0, ETH_ALEN);
			sip_send_bss_info_update(epub, evif, bssid, assoc);
		}
	} else if (vif->type == NL80211_IFTYPE_AP) {
		if (!(changed & BSS_CHANGED_BEACON_ENABLED) &&
		    !(changed & BSS_CHANGED_BEACON_INT))
			return;

		if (info->enable_beacon && !evif->ap_up) {
			evif->beacon_interval = info->beacon_int;
			init_beacon_timer(vif);
			sip_send_bss_info_update(epub, evif, bssid, 2);
			evif->ap_up = true;
		} else if (!info->enable_beacon && evif->ap_up &&
			   !(hw->conf.flags & IEEE80211_CONF_OFFCHANNEL)) {
			evif->beacon_interval = 0;
			del_timer_sync(&evif->beacon_timer);
			sip_send_bss_info_update(epub, evif, bssid, 2);
			evif->ap_up = false;
		}
	}
}

/*
   Configure the device's RX filter.
   3015  *      See the section "Frame filtering" for more information.
   3016  *      This callback must be implemented and can sleep.
 */
static void esp_op_configure_filter(struct ieee80211_hw *hw,
                                    unsigned int changed_flags,
                                    unsigned int *total_flags,
                                    u64 multicast)
{
        struct esp_pub *epub = (struct esp_pub *)hw->priv;

        ESP_IEEE80211_DBG(ESP_DBG_TRACE, "%s enter \n", __func__);

        epub->rx_filter = 0;

        if (*total_flags & FIF_ALLMULTI)
                epub->rx_filter |= FIF_ALLMULTI;

        *total_flags = epub->rx_filter;
}

static bool is_cipher_suite_wep(u32 cipher)
{
	return (cipher == WLAN_CIPHER_SUITE_WEP40) ||
		(cipher == WLAN_CIPHER_SUITE_WEP104);
}

/*
   See the section "Hardware crypto acceleration"
   3029  *      This callback is only called between add_interface and
   3030  *      remove_interface calls, i.e. while the given virtual interface
   3031  *      is enabled.
   3032  *      Returns a negative error code if the key can't be added.
   3033  *      The callback can sleep.
 */
static int esp_op_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
                          struct ieee80211_vif *vif, struct ieee80211_sta *sta,
                          struct ieee80211_key_conf *key)
{
	struct esp_pub *epub = (struct esp_pub *)hw->priv;
	struct esp_vif *evif = (struct esp_vif *)vif->drv_priv;
	struct esp_hw_idx_map *map;
	atomic_t *cnt1, *cnt2;
	u8 i, ifidx = evif->index, isvalid, index;
	u8 *peer_addr;
	int ret, counter;

        ESP_IEEE80211_DBG(ESP_DBG_OP, "%s enter, flags = %x keyindx = %x cmd = %x mac = %pM cipher = %x\n", __func__, key->flags, key->keyidx, cmd, vif->addr, key->cipher);

	key->flags |= IEEE80211_KEY_FLAG_GENERATE_IV;

	if (sta && memcmp(sta->addr, epub->wl.bssid, ETH_ALEN))
		peer_addr = sta->addr;
	else
		peer_addr = epub->wl.bssid;

	isvalid = !!(cmd == SET_KEY);

	if (key->flags & IEEE80211_KEY_FLAG_PAIRWISE ||
	    is_cipher_suite_wep(key->cipher))
		map = epub->low_map[ifidx];
	else
		map = epub->hi_map;

	if (isvalid) {
		if (key->flags & IEEE80211_KEY_FLAG_PAIRWISE ||
		    is_cipher_suite_wep(key->cipher))
			counter = 2;
		else
			counter = 19;

		for (i = 0; i < counter; i++) {
			if (map[i].flag)
				continue;

			map[i].flag = 1;
			memcpy(map[i].mac, peer_addr, ETH_ALEN);
			if (key->flags & IEEE80211_KEY_FLAG_PAIRWISE ||
			    is_cipher_suite_wep(key->cipher))
				key->hw_key_idx = i + 6;
			else
				key->hw_key_idx = i + ifidx * 2 + 2;
			break;
		}
	} else {
		map[ifidx].flag = 0;
		memset(map[ifidx].mac, 0, ETH_ALEN);

		if (key->flags & IEEE80211_KEY_FLAG_PAIRWISE ||
		    is_cipher_suite_wep(key->cipher))
			index = key->hw_key_idx - 6;
		else
			index = key->hw_key_idx - 2 - ifidx * 2;
	}

	if (key->hw_key_idx >= 6) {
		cnt1 = &epub->wl.ptk_cnt;
		cnt2 = &epub->wl.gtk_cnt;
	} else {
		cnt2 = &epub->wl.ptk_cnt;
		cnt1 = &epub->wl.gtk_cnt;
	}

	/*send sub_scan task to target */
	if (isvalid)
		atomic_inc(cnt1);
	else
		atomic_dec(cnt1);

	if (is_cipher_suite_wep(key->cipher)) {
		if (isvalid)
			atomic_inc(cnt2);
		else
			atomic_dec(cnt2);
	}

	ret = sip_send_setkey(epub, ifidx, peer_addr, key, isvalid);
	if (ret)
		return ret;

	if (key->cipher == WLAN_CIPHER_SUITE_TKIP && !ret)
		atomic_set(&epub->wl.tkip_key_set, 1);

	return 0;
}

void hw_scan_done(struct esp_pub *epub, bool aborted)
{

        struct cfg80211_scan_info info = {
            .aborted = aborted,
        };
        
        cancel_delayed_work_sync(&epub->scan_timeout_work);

        ESSERT(epub->wl.scan_req);
       
        ieee80211_scan_completed(epub->hw, &info); 

        if (test_and_clear_bit(ESP_WL_FLAG_STOP_TXQ, &epub->wl.flags)) 
                sip_trigger_txq_process(epub->sip);      
}

static void hw_scan_timeout_report(struct work_struct *work)
{
	struct esp_pub *epub = container_of(work, struct esp_pub,
					    scan_timeout_work.work);
	bool aborted;
	struct cfg80211_scan_info info = {};

	if (test_and_clear_bit(ESP_WL_FLAG_STOP_TXQ, &epub->wl.flags))
		sip_trigger_txq_process(epub->sip);
	/*check if normally complete or aborted like timeout/hw error */
	aborted = (epub->wl.scan_req != NULL);

	if (aborted)
		epub->wl.scan_req = NULL;

	info.aborted = aborted;

	ieee80211_scan_completed(epub->hw, &info);
}

/*
   Configuration of RTS threshold (if device needs it)
   3106  *      The callback can sleep.
   */
static int esp_op_set_rts_threshold(struct ieee80211_hw *hw, u32 value)
{
        ESP_IEEE80211_DBG(ESP_DBG_TRACE, "%s enter \n", __func__);
        return 0;
}

static int esp_node_attach(struct ieee80211_hw *hw, u8 ifidx, 
			struct ieee80211_sta *sta)
{
	struct esp_pub *epub = (struct esp_pub *)hw->priv;
	struct esp_node *node;
	struct esp_tx_tid *tid;
	u8 tidno = 0;
	int i;

	spin_lock_bh(&epub->tx_ampdu_lock);

	/* ffz(x) needs at least one zero or results in undefined behaviour. */
	if ((~epub->enodes_map) == 0)
		return -EINVAL;

	i = ffz(epub->enodes_map);

	if (hweight32(epub->enodes_maps[ifidx]) >= ESP_PUB_MAX_STA ||
	    i > ESP_PUB_MAX_STA) {
		i = -1;
		goto out;
	}

	epub->enodes_map |= BIT(i);
	epub->enodes_maps[ifidx] |= BIT(i);
	node = (struct esp_node *)sta->drv_priv;
	epub->enodes[i] = node;
	node->sta = sta;
	node->ifidx = ifidx;
	node->index = i;

	while (tidno < WME_NUM_TID) {
		tid = &node->tid[tidno];
		tid->ssn = 0;
		tid->cnt = 0;
		tid->state = ESP_TID_STATE_INIT;
		tidno++;
	}

out:
	spin_unlock_bh(&epub->tx_ampdu_lock);

	return i;
}


static int esp_node_detach(struct ieee80211_hw *hw, u8 ifidx,
				struct ieee80211_sta *sta)
{
    struct esp_pub *epub = (struct esp_pub *)hw->priv;
	u32 map;
	int i;

	spin_lock_bh(&epub->tx_ampdu_lock);
	
	map = epub->enodes_maps[ifidx];
	
	while (map) {
		i = ffs(map) - 1;
		if(epub->enodes[i]->sta == sta){
			epub->enodes[i]->sta = NULL;
			epub->enodes[i] = NULL;
			epub->enodes_map &= ~BIT(i);
			epub->enodes_maps[ifidx] &= ~BIT(i);

			goto out;

		}
		map &= ~BIT(i);
	}
	
	i = -1;
	
out:
	spin_unlock_bh(&epub->tx_ampdu_lock);

	return i;
}

struct esp_node * esp_get_node_by_addr(struct esp_pub * epub, const u8 *addr)
{
	struct esp_node *node = NULL;
	int i;
	u32 map;
	
	if (!addr)
		return NULL;
		
	spin_lock_bh(&epub->tx_ampdu_lock);
	map = epub->enodes_map;
	
	while(map) {
		i = ffs(map) - 1;		

		if (!memcmp(epub->enodes[i]->sta->addr, addr, ETH_ALEN)) {
			node = epub->enodes[i];
			goto out;
		}

		map &= ~BIT(i);
	}

out:
	spin_unlock_bh(&epub->tx_ampdu_lock);

	return node;
}

struct esp_node * esp_get_node_by_index(struct esp_pub * epub, u8 index)
{
	u32 map;
	struct esp_node *node = NULL;

	if (epub == NULL)
		return NULL;

	spin_lock_bh(&epub->tx_ampdu_lock);
	map = epub->enodes_map;
	if (map & BIT(index)) {
		node = epub->enodes[index];
	} else {
		spin_unlock_bh(&epub->tx_ampdu_lock);
		return NULL;
	}

	spin_unlock_bh(&epub->tx_ampdu_lock);
	return node;
}

int esp_get_empty_rxampdu(struct esp_pub *epub, const u8 *addr, u8 tid)
{
	int index;
	
	if (!addr)
		return -1;
		
	spin_lock_bh(&epub->rx_ampdu_lock);
	
	index = ffz(epub->rxampdu_map);

	if (index >= ESP_PUB_MAX_RXAMPDU) {
		index = -1;
		goto out;
	}

	epub->rxampdu_map |= BIT(index);
	epub->rxampdu_node[index] = esp_get_node_by_addr(epub, addr);
	epub->rxampdu_tid[index] = tid;

out:
	spin_unlock_bh(&epub->rx_ampdu_lock);

	return index;
}

int esp_get_exist_rxampdu(struct esp_pub * epub, const u8 *addr, u8 tid)
{
	u8 map;
	int index;

	if (!addr)
		return -1;

	spin_lock_bh(&epub->rx_ampdu_lock);
	map = epub->rxampdu_map;

	while (map) {
		index = ffs(map) - 1;

		if (epub->rxampdu_tid[index] == tid &&
		    !memcmp(epub->rxampdu_node[index]->sta->addr, addr,
			    ETH_ALEN)) {
			epub->rxampdu_map &= ~BIT(index);
			goto out;
		}

		map &= ~BIT(index);
	}

	index = -1;

out:
	spin_unlock_bh(&epub->rx_ampdu_lock);
	return index;
}

/*
   Notifies low level driver about addition of an associated station,
   3109  *      AP, IBSS/WDS/mesh peer etc. This callback can sleep.
   */
static int esp_op_sta_add(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
 				struct ieee80211_sta *sta)
{
	struct esp_pub *epub = (struct esp_pub *)hw->priv;
	struct esp_vif *evif = (struct esp_vif *)vif->drv_priv;
	int index;

	if (vif->type == NL80211_IFTYPE_STATION)
		reset_signal_count();

	ESP_IEEE80211_DBG(ESP_DBG_OP, "%s enter, vif addr %pM, sta addr %pM\n", __func__, vif->addr, sta->addr);
	index = esp_node_attach(hw, evif->index, sta);
	if(index < 0)
		return index;
		
	sip_send_set_sta(epub, evif->index, 1, sta, vif, (u8)index);

    	return 0;
}

/*
 Notifies low level driver about removal of an associated
 3112  *      station, AP, IBSS/WDS/mesh peer etc. Note that after the callback
 3113  *      returns it isn't safe to use the pointer, not even RCU protected;
 3114  *      no RCU grace period is guaranteed between returning here and freeing
 3115  *      the station. See @sta_pre_rcu_remove if needed.
 3116  *      This callback can sleep
 */
static int esp_op_sta_remove(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
 				struct ieee80211_sta *sta)
{	
	struct esp_pub *epub = (struct esp_pub *)hw->priv;
	struct esp_vif *evif = (struct esp_vif *)vif->drv_priv;
	int index;

	ESP_IEEE80211_DBG(ESP_DBG_OP, "%s enter, vif addr %pM, sta addr %pM\n", __func__, vif->addr, sta->addr);
	
    	//remove a connect in target
	index = esp_node_detach(hw, evif->index, sta);
	sip_send_set_sta(epub, evif->index, 0, sta, vif, (u8)index);

	return 0;
}

/*
 Notifies low level driver about power state transition of an
 3124  *      associated station, AP,  IBSS/WDS/mesh peer etc. For a VIF operating
 3125  *      in AP mode, this callback will not be called when the flag
 3126  *      %IEEE80211_HW_AP_LINK_PS is set. Must be atomic.
 */
static void esp_op_sta_notify(struct ieee80211_hw *hw,
			       struct ieee80211_vif *vif,
 			       enum sta_notify_cmd cmd,
 			       struct ieee80211_sta *sta)
{
        ESP_IEEE80211_DBG(ESP_DBG_TRACE, "%s enter \n", __func__);
}

/*
 Configure TX queue parameters (EDCF (aifs, cw_min, cw_max),
 3165  *      bursting) for a hardware TX queue.
 3166  *      Returns a negative error code on failure.
 3167  *      The callback can sleep.
 */
static int esp_op_conf_tx(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			  u16 queue,
                         const struct ieee80211_tx_queue_params *params)
{
        struct esp_pub *epub = (struct esp_pub *)hw->priv;
        ESP_IEEE80211_DBG(ESP_DBG_TRACE, "%s enter \n", __func__);
        return sip_send_wmm_params(epub, queue, params);
}

/*
 Get the current TSF timer value from firmware/hardware. Currently,
 3170  *      this is only used for IBSS mode BSSID merging and debugging. Is not a
 3171  *      required function.
 3172  *      The callback can sleep.
 */
static u64 esp_op_get_tsf(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
        ESP_IEEE80211_DBG(ESP_DBG_TRACE, "%s enter \n", __func__);
        return 0;
}

/*
 Set the TSF timer to the specified value in the firmware/hardware.
 3175  *      Currently, this is only used for IBSS mode debugging. Is not a
 3176  *      required function.
 3177  *      The callback can sleep.
 */
static void esp_op_set_tsf(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			    u64 tsf)
{
        ESP_IEEE80211_DBG(ESP_DBG_TRACE, "%s enter \n", __func__);
}

/*
 Reset the TSF timer and allow firmware/hardware to synchronize
 3186  *      with other STAs in the IBSS. This is only used in IBSS mode. This
 3187  *      function is optional if the firmware/hardware takes full care of
 3188  *      TSF synchronization.
 3189  *      The callback can sleep.
 */
static void esp_op_reset_tsf(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
        ESP_IEEE80211_DBG(ESP_DBG_TRACE, "%s enter \n", __func__);
}

/*
 Poll rfkill hardware state. If you need this, you also
 3220  *      need to set wiphy->rfkill_poll to %true before registration,
 3221  *      and need to call wiphy_rfkill_set_hw_state() in the callback.
 3222  *      The callback can sleep.
 */
static void esp_op_rfkill_poll(struct ieee80211_hw *hw)
{
        struct esp_pub *epub = (struct esp_pub *)hw->priv;

        ESP_IEEE80211_DBG(ESP_DBG_TRACE, "%s enter \n", __func__);

        wiphy_rfkill_set_hw_state(hw->wiphy,
                                  test_bit(ESP_WL_FLAG_RFKILL, &epub->wl.flags));
}

#ifdef HW_SCAN
/*
 Ask the hardware to service the scan request, no need to start
 3051  *      the scan state machine in stack. The scan must honour the channel
 3052  *      configuration done by the regulatory agent in the wiphy's
 3053  *      registered bands. The hardware (or the driver) needs to make sure
 3054  *      that power save is disabled.
 3055  *      The @req ie/ie_len members are rewritten by mac80211 to contain the
 3056  *      entire IEs after the SSID, so that drivers need not look at these
 3057  *      at all but just send them after the SSID -- mac80211 includes the
 3058  *      (extended) supported rates and HT information (where applicable).
 3059  *      When the scan finishes, ieee80211_scan_completed() must be called;
 3060  *      note that it also must be called when the scan cannot finish due to
 3061  *      any error unless this callback returned a negative error code.
 3062  *      The callback can sleep.
 */
static int esp_op_hw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			  struct cfg80211_scan_request *req)
{
	struct esp_pub *epub = (struct esp_pub *)hw->priv;
	struct cfg80211_ssid *ssid2 = req->ssids + 1;
	int i, ret;
	bool scan_often;

	/* scan_request is keep allocate until scan_done,record it
	 * to split request into multi sdio_cmd
	 */
	if (atomic_read(&epub->wl.off)) {
		dev_err(epub->dev, "hw_scan but wl off\n");
		return -EPERM;
	}

	if (req->n_ssids > 1)
		if ((req->ssids->ssid_len > 0 && ssid2->ssid_len > 0) ||
		    req->n_ssids > 2) {
			dev_err(epub->dev, "cannot scan two SSIDs\n");
			return -EINVAL;
		}

	epub->wl.scan_req = req;

	/*in connect state, suspend tx data */
	if (epub->sip->support_bgscan &&
	    test_bit(ESP_WL_FLAG_CONNECT, &epub->wl.flags) && req->n_channels) {
		scan_often = epub->scan_permit_valid &&
			time_before(jiffies, epub->scan_permit);
		epub->scan_permit_valid = true;

		if (!scan_often) {
			/* epub->scan_permit = jiffies + msecs_to_jiffies(900);
			 *  set_bit(ESP_WL_FLAG_STOP_TXQ, &epub->wl.flags);
			 *  if (atomic_read(&epub->txq_stopped) == false) {
			 *  atomic_set(&epub->txq_stopped, true);
			 *  ieee80211_stop_queues(hw);
			 *  }
			 */
		} else {
			dev_err(epub->dev, "scan too often\n");
			return -EACCES;
		}
	} else {
		scan_often = false;
	}

	/*send sub_scan task to target */
	ret = sip_send_scan(epub);
	if (ret) {
		dev_err(epub->dev, "failed to send scan_cmd: %d\n", ret);
		return ret;
	}

	if (scan_often)
		return 0;

	epub->scan_permit = jiffies + msecs_to_jiffies(900);
	set_bit(ESP_WL_FLAG_STOP_TXQ, &epub->wl.flags);
	if (!atomic_read(&epub->txq_stopped)) {
		atomic_set(&epub->txq_stopped, true);
		ieee80211_stop_queues(hw);
	}

	/*force scan complete in case target fail to report in time */
	ieee80211_queue_delayed_work(hw, &epub->scan_timeout_work,
				     req->n_channels * HZ / 4);

	return 0;
}

/*
 Starts an off-channel period on the given channel, must
 3255  *      call back to ieee80211_ready_on_channel() when on that channel. Note
 3256  *      that normal channel traffic is not stopped as this is intended for hw
 3257  *      offload. Frames to transmit on the off-channel channel are transmitted
 3258  *      normally except for the %IEEE80211_TX_CTL_TX_OFFCHAN flag. When the
 3259  *      duration (which will always be non-zero) expires, the driver must call
 3260  *      ieee80211_remain_on_channel_expired().
 3261  *      Note that this callback may be called while the device is in IDLE and
 3262  *      must be accepted in this case.
 3263  *      This callback may sleep.
 */
static int esp_op_remain_on_channel(struct ieee80211_hw *hw,
                                    struct ieee80211_channel *chan,
                                    enum nl80211_channel_type channel_type,
                                    int duration)
{
      struct esp_pub *epub = (struct esp_pub *)hw->priv;

      ESP_IEEE80211_DBG(ESP_DBG_OP, "%s enter, center_freq = %d duration = %d\n", __func__, chan->center_freq, duration);
      
      sip_send_roc(epub, chan->center_freq, duration);
      
      return 0;
}

/*
 3264  * @cancel_remain_on_channel: Requests that an ongoing off-channel period is
 3265  *      aborted before it expires. This callback may sleep.
 */
static int esp_op_cancel_remain_on_channel(struct ieee80211_hw *hw)
{
      struct esp_pub *epub = (struct esp_pub *)hw->priv;

      ESP_IEEE80211_DBG(ESP_DBG_OP, "%s enter \n", __func__);
      epub->roc_flags= 0;  // to disable roc state
      sip_send_roc(epub, 0, 0);
      
     return 0;
}
#endif

void esp_rocdone_process(struct ieee80211_hw *hw, struct sip_evt_roc *report)
{    
      struct esp_pub *epub = (struct esp_pub *)hw->priv;

      ESP_IEEE80211_DBG(ESP_DBG_OP, "%s enter, state = %d is_ok = %d\n", __func__, report->state, report->is_ok);

	if (report->is_ok != 1)
		return;

      if (report->state == 1) {
           epub->roc_flags=1;
           ieee80211_ready_on_channel(hw);
      } else if (!report->state) {
           epub->roc_flags= 0;
           ieee80211_remain_on_channel_expired(hw);     
       }
}

/*
 Set a mask of rates to be used for rate control selection
 3275  *      when transmitting a frame. Currently only legacy rates are handled.
 3276  *      The callback can sleep.
 */
static int esp_op_set_bitrate_mask(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				const struct cfg80211_bitrate_mask *mask)
{
        ESP_IEEE80211_DBG(ESP_DBG_OP, "%s enter \n", __func__);
        ESP_IEEE80211_DBG(ESP_DBG_OP, "%s vif->macaddr[%pM], mask[%d]\n", __func__, vif->addr, mask->control[0].legacy);
	return 0;
}

/*
 Flush all pending frames from the hardware queue, making sure
 3235  *      that the hardware queues are empty. The @queues parameter is a bitmap
 3236  *      of queues to flush, which is useful if different virtual interfaces
 3237  *      use different hardware queues; it may also indicate all queues.
 3238  *      If the parameter @drop is set to %true, pending frames may be dropped.
 3239  *      Note that vif can be NULL.
 3240  *      The callback can sleep.
 */
void esp_op_flush(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
 			u32 queues, bool drop)
{
	struct esp_pub *epub = (struct esp_pub *)hw->priv;
	unsigned long time = jiffies + msecs_to_jiffies(15);

	while (atomic_read(&epub->sip->tx_data_pkt_queued)) {
		if (!time_before(jiffies, time))
			break;

		if (!sif_get_ate_config())
			ieee80211_queue_work(epub->hw, &epub->tx_work);
		else
			queue_work(epub->esp_wkq, &epub->tx_work);
	}

	mdelay(10);
}

/*
 Perform a certain A-MPDU action
 3198  *      The RA/TID combination determines the destination and TID we want
 3199  *      the ampdu action to be performed for. The action is defined through
 3200  *      ieee80211_ampdu_mlme_action.
 3201  *      When the action is set to %IEEE80211_AMPDU_TX_OPERATIONAL the driver
 3202  *      may neither send aggregates containing more subframes than @buf_size
 3203  *      nor send aggregates in a way that lost frames would exceed the
 3204  *      buffer size. If just limiting the aggregate size, this would be
 3205  *      possible with a buf_size of 8:
 3206  *       - TX: 1.....7
 3207  *       - RX:  2....7 (lost frame #1)
 3208  *       - TX:        8..1...
 3209  *      which is invalid since #1 was now re-transmitted well past the
 3210  *      buffer size of 8. Correct ways to retransmit #1 would be:
 3211  *       - TX:       1 or 18 or 81
 3212  *      Even "189" would be wrong since 1 could be lost again.
 3213  *
 3214  *      Returns a negative error code on failure.
 3215  *      The callback can sleep.
 */
static int esp_op_ampdu_action(struct ieee80211_hw *hw,
			       struct ieee80211_vif *vif,
			       struct ieee80211_ampdu_params *params)
{
	enum ieee80211_ampdu_mlme_action action = params->action;
	struct ieee80211_sta *sta = params->sta;
	struct esp_pub *epub = (struct esp_pub *)hw->priv;
	struct esp_node *node = (struct esp_node *)sta->drv_priv;
	struct cfg80211_chan_def *chandef;
	u16 tid = params->tid;
	struct esp_tx_tid *tid_info = &node->tid[tid];
	u16 *ssn = &params->ssn;
	u8 buf_size = params->buf_size;

	switch (action) {
	case IEEE80211_AMPDU_TX_START:
		chandef = &epub->hw->conf.chandef;
		if (mod_support_no_txampdu() || !sta->ht_cap.ht_supported ||
		    cfg80211_get_chandef_type(chandef) == NL80211_CHAN_NO_HT)
			return -EOPNOTSUPP;

		dev_dbg(epub->dev, "%s TX START, addr:%pM,tid:%u,state:%d\n",
			__func__, sta->addr, tid, tid_info->state);

		spin_lock_bh(&epub->tx_ampdu_lock);

		ESSERT(tid_info->state == ESP_TID_STATE_TRIGGER);
		*ssn = tid_info->ssn;
		tid_info->state = ESP_TID_STATE_PROGRESS;

		ieee80211_start_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		spin_unlock_bh(&epub->tx_ampdu_lock);

		return 0;

	case IEEE80211_AMPDU_TX_STOP_CONT:
		dev_dbg(epub->dev, "%s TX STOP, addr:%pM,tid:%u,state:%d\n",
			__func__, sta->addr, tid, tid_info->state);

		spin_lock_bh(&epub->tx_ampdu_lock);

	case IEEE80211_AMPDU_TX_STOP_FLUSH:
	case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
		if (tid_info->state == ESP_TID_STATE_WAIT_STOP)
			tid_info->state = ESP_TID_STATE_STOP;
		else
			tid_info->state = ESP_TID_STATE_INIT;

		if (action == IEEE80211_AMPDU_TX_STOP_CONT) {
			ieee80211_stop_tx_ba_cb_irqsafe(vif, sta->addr, tid);
			spin_unlock_bh(&epub->tx_ampdu_lock);
		}

		return sip_send_ampdu_action(epub, SIP_AMPDU_TX_STOP, sta->addr,
					     tid, node->ifidx, 0);

	case IEEE80211_AMPDU_TX_OPERATIONAL:
		dev_dbg(epub->dev,
			"%s TX OPERATION, addr:%pM,tid:%u,state:%d\n", __func__,
			sta->addr, tid, tid_info->state);

		spin_lock_bh(&epub->tx_ampdu_lock);

		if (tid_info->state != ESP_TID_STATE_PROGRESS) {
			if (tid_info->state == ESP_TID_STATE_INIT) {
				printk(KERN_ERR "%s WIFI RESET, IGNORE\n",
				       __func__);
				spin_unlock_bh(&epub->tx_ampdu_lock);
				return -ENETRESET;
			}

			ESSERT(0);
		}

		tid_info->state = ESP_TID_STATE_OPERATIONAL;
		spin_unlock_bh(&epub->tx_ampdu_lock);

		return sip_send_ampdu_action(epub, SIP_AMPDU_TX_OPERATIONAL,
					     sta->addr, tid, node->ifidx,
					     buf_size);

	case IEEE80211_AMPDU_RX_START:
		chandef = &epub->hw->conf.chandef;
		if (mod_support_no_rxampdu() || !sta->ht_cap.ht_supported ||
		    cfg80211_get_chandef_type(chandef) == NL80211_CHAN_NO_HT)
			return -EOPNOTSUPP;

		dev_dbg(epub->dev, "%s RX START %pM tid %u %u\n", __func__,
			sta->addr, tid, *ssn);

		return sip_send_ampdu_action(epub, SIP_AMPDU_RX_START, sta->addr,
					     tid, *ssn, 64);

	case IEEE80211_AMPDU_RX_STOP:
		dev_dbg(epub->dev, "%s RX STOP %pM tid %u\n", __func__,
			sta->addr, tid);

		return sip_send_ampdu_action(epub, SIP_AMPDU_RX_STOP, sta->addr,
					     tid, 0, 0);

	default:
		return -EINVAL;
	}
}

static void esp_tx_work(struct work_struct *work)
{
        struct esp_pub *epub = container_of(work, struct esp_pub, tx_work);

        mutex_lock(&epub->tx_mtx);
        sip_txq_process(epub);
        mutex_unlock(&epub->tx_mtx);
}

#ifndef RX_SENDUP_SYNC
//for debug
static int data_pkt_dequeue_cnt = 0;
static void _esp_flush_rxq(struct esp_pub *epub)
{
        struct sk_buff *skb = NULL;

        while ((skb = skb_dequeue(&epub->rxq))) {
		//do not log when in spin_lock
                //esp_dbg(ESP_DBG_TRACE, "%s call ieee80211_rx \n", __func__);
                ieee80211_rx(epub->hw, skb);
        }
}

static void
esp_sendup_work(struct work_struct *work)
{
        struct esp_pub *epub = container_of(work, struct esp_pub, sendup_work);
        spin_lock_bh(&epub->rx_lock);
        _esp_flush_rxq(epub);
        spin_unlock_bh(&epub->rx_lock);
}
#endif /* !RX_SENDUP_SYNC */

static const struct ieee80211_ops esp_mac80211_ops = {
        .tx = esp_op_tx,
        .start = esp_op_start,
        .stop = esp_op_stop,
        .add_interface = esp_op_add_interface,
        .remove_interface = esp_op_remove_interface,
        .config = esp_op_config,
        .bss_info_changed = esp_op_bss_info_changed,
        .configure_filter = esp_op_configure_filter,
        .set_key = esp_op_set_key,
        .set_rts_threshold = esp_op_set_rts_threshold,
        .sta_notify = esp_op_sta_notify,
        .conf_tx = esp_op_conf_tx,
	.change_interface = esp_op_change_interface,
        .get_tsf = esp_op_get_tsf,
        .set_tsf = esp_op_set_tsf,
        .reset_tsf = esp_op_reset_tsf,
        .rfkill_poll= esp_op_rfkill_poll,
#ifdef HW_SCAN
        .hw_scan = esp_op_hw_scan,
        .remain_on_channel= esp_op_remain_on_channel,
        .cancel_remain_on_channel=esp_op_cancel_remain_on_channel,
#endif
        .ampdu_action = esp_op_ampdu_action,
        .sta_add = esp_op_sta_add,
        .sta_remove = esp_op_sta_remove,
	.set_bitrate_mask = esp_op_set_bitrate_mask,
	.flush = esp_op_flush,
};

struct esp_pub *esp_pub_alloc_mac80211(struct device *dev)
{
        struct ieee80211_hw *hw;
        struct esp_pub *epub;

        hw = ieee80211_alloc_hw(sizeof(struct esp_pub), &esp_mac80211_ops);
        if (!hw) {
                esp_dbg(ESP_DBG_ERROR, "ieee80211 can't alloc hw!\n");
                return ERR_PTR(-ENOMEM);
        }

	/* FIXME: useless if hw_scan is defined, incorrect if hw_scan is undefined*/
#ifdef HW_SCAN
        hw->wiphy->flags |= WIPHY_FLAG_HAS_REMAIN_ON_CHANNEL;
#endif

        epub = hw->priv;
        memset(epub, 0, sizeof(*epub));
        
        epub->hw = hw;
        SET_IEEE80211_DEV(hw, dev);
        epub->dev = dev;

        skb_queue_head_init(&epub->txq);
        skb_queue_head_init(&epub->txdoneq);
        skb_queue_head_init(&epub->rxq);

	spin_lock_init(&epub->tx_ampdu_lock);
	spin_lock_init(&epub->rx_ampdu_lock);
        spin_lock_init(&epub->tx_lock);
        mutex_init(&epub->tx_mtx);
        spin_lock_init(&epub->rx_lock);

        INIT_WORK(&epub->tx_work, esp_tx_work);
        
#ifndef RX_SENDUP_SYNC
        INIT_WORK(&epub->sendup_work, esp_sendup_work);
#endif //!RX_SENDUP_SYNC

        epub->esp_wkq = create_singlethread_workqueue("esp_wkq");

        if (!epub->esp_wkq) {
                return ERR_PTR(-ENOMEM);
        }

	epub->master_ifidx = ESP_PUB_MAX_VIF;

        epub->scan_permit_valid = false;
        INIT_DELAYED_WORK(&epub->scan_timeout_work, hw_scan_timeout_report);

        return epub;
}


int esp_pub_dealloc_mac80211(struct esp_pub *epub)
{
        set_bit(ESP_WL_FLAG_RFKILL, &epub->wl.flags);

        destroy_workqueue(epub->esp_wkq);
        mutex_destroy(&epub->tx_mtx);

#ifdef ESP_NO_MAC80211
        free_netdev(epub->net_dev);
        wiphy_free(epub->wdev->wiphy);
        kfree(epub->wdev);
#else
        if (epub->hw) {
                ieee80211_free_hw(epub->hw);
        }
#endif

        return 0;
}

/* 2G band channels */
static struct ieee80211_channel esp_channels_2ghz[] = {
        { .hw_value = 1, .center_freq = 2412, .max_power = 25 },
        { .hw_value = 2, .center_freq = 2417, .max_power = 25 },
        { .hw_value = 3, .center_freq = 2422, .max_power = 25 },
        { .hw_value = 4, .center_freq = 2427, .max_power = 25 },
        { .hw_value = 5, .center_freq = 2432, .max_power = 25 },
        { .hw_value = 6, .center_freq = 2437, .max_power = 25 },
        { .hw_value = 7, .center_freq = 2442, .max_power = 25 },
        { .hw_value = 8, .center_freq = 2447, .max_power = 25 },
        { .hw_value = 9, .center_freq = 2452, .max_power = 25 },
        { .hw_value = 10, .center_freq = 2457, .max_power = 25 },
        { .hw_value = 11, .center_freq = 2462, .max_power = 25 },
        { .hw_value = 12, .center_freq = 2467, .max_power = 25 },
        { .hw_value = 13, .center_freq = 2472, .max_power = 25 },
        { .hw_value = 14, .center_freq = 2484, .max_power = 25 },
};

/* 11G rate */
static struct ieee80211_rate esp_rates_2ghz[] = {
        {
                .bitrate = 10,
                .hw_value = CONF_HW_BIT_RATE_1MBPS,
                .hw_value_short = CONF_HW_BIT_RATE_1MBPS,
        },
        {
                .bitrate = 20,
                .hw_value = CONF_HW_BIT_RATE_2MBPS,
                .hw_value_short = CONF_HW_BIT_RATE_2MBPS,
                .flags = IEEE80211_RATE_SHORT_PREAMBLE
        },
        {
                .bitrate = 55,
                .hw_value = CONF_HW_BIT_RATE_5_5MBPS,
                .hw_value_short = CONF_HW_BIT_RATE_5_5MBPS,
                .flags = IEEE80211_RATE_SHORT_PREAMBLE
        },
        {
                .bitrate = 110,
                .hw_value = CONF_HW_BIT_RATE_11MBPS,
                .hw_value_short = CONF_HW_BIT_RATE_11MBPS,
                .flags = IEEE80211_RATE_SHORT_PREAMBLE
        },
        {
                .bitrate = 60,
                .hw_value = CONF_HW_BIT_RATE_6MBPS,
                .hw_value_short = CONF_HW_BIT_RATE_6MBPS,
        },
        {
                .bitrate = 90,
                .hw_value = CONF_HW_BIT_RATE_9MBPS,
                .hw_value_short = CONF_HW_BIT_RATE_9MBPS,
        },
        {
                .bitrate = 120,
                .hw_value = CONF_HW_BIT_RATE_12MBPS,
                .hw_value_short = CONF_HW_BIT_RATE_12MBPS,
        },
        {
                .bitrate = 180,
                .hw_value = CONF_HW_BIT_RATE_18MBPS,
                .hw_value_short = CONF_HW_BIT_RATE_18MBPS,
        },
        {
                .bitrate = 240,
                .hw_value = CONF_HW_BIT_RATE_24MBPS,
                .hw_value_short = CONF_HW_BIT_RATE_24MBPS,
        },
        {
                .bitrate = 360,
                .hw_value = CONF_HW_BIT_RATE_36MBPS,
                .hw_value_short = CONF_HW_BIT_RATE_36MBPS,
        },
        {
                .bitrate = 480,
                .hw_value = CONF_HW_BIT_RATE_48MBPS,
                .hw_value_short = CONF_HW_BIT_RATE_48MBPS,
        },
        {
                .bitrate = 540,
                .hw_value = CONF_HW_BIT_RATE_54MBPS,
                .hw_value_short = CONF_HW_BIT_RATE_54MBPS,
        },
};

static struct ieee80211_sta_ht_cap esp_ht_cap_2ghz = {
	.cap = IEEE80211_HT_CAP_DSSSCCK40 | IEEE80211_HT_CAP_SM_PS |
		IEEE80211_HT_CAP_SGI_40 | IEEE80211_HT_CAP_SGI_20,
	.ht_supported = true,
	.ampdu_factor = IEEE80211_HT_MAX_AMPDU_16K,
	.ampdu_density = IEEE80211_HT_MPDU_DENSITY_NONE,
	.mcs = {
		.rx_mask = { 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
	},
};

static void esp_pub_init_mac80211(struct esp_pub *epub)
{
	struct ieee80211_hw *hw = epub->hw;
	struct ieee80211_supported_band *sbands =
		&epub->wl.sbands[NL80211_BAND_2GHZ];

	static const u32 cipher_suites[] = {
		WLAN_CIPHER_SUITE_WEP40,
		WLAN_CIPHER_SUITE_WEP104,
		WLAN_CIPHER_SUITE_TKIP,
		WLAN_CIPHER_SUITE_CCMP,
	};

	hw->max_listen_interval = 10;

	ieee80211_hw_set(hw, SIGNAL_DBM);
	ieee80211_hw_set(hw, HAS_RATE_CONTROL);
	ieee80211_hw_set(hw, SUPPORTS_PS);
	ieee80211_hw_set(hw, AMPDU_AGGREGATION);
	ieee80211_hw_set(hw, HOST_BROADCAST_PS_BUFFERING);
	hw->max_rx_aggregation_subframes = IEEE80211_MAX_AMPDU_BUF;
	hw->max_tx_aggregation_subframes = IEEE80211_MAX_AMPDU_BUF;

	hw->wiphy->cipher_suites = cipher_suites;
	hw->wiphy->n_cipher_suites = ARRAY_SIZE(cipher_suites);
	hw->wiphy->max_scan_ie_len = epub->sip->tx_blksz -
		sizeof(struct sip_hdr) - sizeof(struct sip_cmd_scan);

	/* ONLY station for now, support P2P soon... */
	/* FIXME: is p2p really supported? */
	hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_P2P_GO) |
		BIT(NL80211_IFTYPE_P2P_CLIENT) | BIT(NL80211_IFTYPE_STATION) |
		BIT(NL80211_IFTYPE_AP);

	hw->wiphy->max_scan_ssids = 2;
	hw->wiphy->max_remain_on_channel_duration = 5000;

	atomic_set(&epub->wl.off, 1);

	sbands->band = NL80211_BAND_2GHZ;
	sbands->channels = esp_channels_2ghz;
	sbands->bitrates = esp_rates_2ghz;
	sbands->n_channels = ARRAY_SIZE(esp_channels_2ghz);
	sbands->n_bitrates = ARRAY_SIZE(esp_rates_2ghz);
	sbands->ht_cap = esp_ht_cap_2ghz;

	hw->wiphy->bands[NL80211_BAND_2GHZ] = sbands;

	/*no fragment */
	hw->wiphy->frag_threshold = IEEE80211_MAX_FRAG_THRESHOLD;

	/* handle AC queue in f/w */
	hw->queues = 4;
	hw->max_rates = 4;

	hw->vif_data_size = sizeof(struct esp_vif);
	hw->sta_data_size = sizeof(struct esp_node);
}

int esp_register_mac80211(struct esp_pub *epub)
{
        int ret;
#ifdef P2P_CONCURRENT
	u8 *wlan_addr;
	u8 *p2p_addr;
	int idx;
#endif

        esp_pub_init_mac80211(epub);

#ifdef P2P_CONCURRENT
	epub->hw->wiphy->addresses = (struct mac_address *)esp_mac_addr;
	memcpy(&epub->hw->wiphy->addresses[0].addr, epub->mac_addr, ETH_ALEN);
	memcpy(&epub->hw->wiphy->addresses[1].addr, epub->mac_addr, ETH_ALEN);
	wlan_addr = epub->hw->wiphy->addresses[0].addr;
	p2p_addr  = epub->hw->wiphy->addresses[1].addr;

	for (idx = 0; idx < 8 * ETH_ALEN; idx++) {
		p2p_addr[0] = epub->mac_addr[0] | 0x02;
		p2p_addr[0] ^= idx << 2;
		if (strncmp(p2p_addr, epub->mac_addr, 6))
			break;
	}

	epub->hw->wiphy->n_addresses = 2;
#else

        SET_IEEE80211_PERM_ADDR(epub->hw, epub->mac_addr);
#endif

        ret = ieee80211_register_hw(epub->hw);
 
        if (ret < 0) {
                printk("unable to register mac80211 hw: %d\n", ret);
                return ret;
        } else {
#ifdef MAC80211_NO_CHANGE
        	rtnl_lock();


if (epub->hw->wiphy->interface_modes &
                (BIT(NL80211_IFTYPE_P2P_GO) | BIT(NL80211_IFTYPE_P2P_CLIENT))) {
                struct vif_params params = {0};        	
        
                ret = ieee80211_if_add(hw_to_local(epub->hw), "p2p%d", NET_NAME_ENUM, NULL,
                                          NL80211_IFTYPE_STATION, &params);
                if (ret)
                        wiphy_warn(epub->hw->wiphy,
                                   "Failed to add default virtual iface\n");
        	}

        	rtnl_unlock();
#endif
	}

        set_bit(ESP_WL_FLAG_HW_REGISTERED, &epub->wl.flags);

        return ret;
}

static u8 getaddr_index(u8 *addr, struct esp_pub *epub)
{
#ifdef P2P_CONCURRENT
	int i;
	
	for(i = 0; i < ESP_PUB_MAX_VIF; i++)
		if (!memcmp(addr, epub->hw->wiphy->addresses[i].addr, ETH_ALEN))
                	return i;
                	
	return ESP_PUB_MAX_VIF;
#else
	return 0;
#endif
}
