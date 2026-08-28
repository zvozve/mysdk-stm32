#include "wol.h"
#include "string.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
#include "SEGGER_RTT_Log.h"

// Use MAC from config_network.h
const uint8_t targetMac[6] = TARGET_PC_MAC_BYTE;

// External LAN network interface (defined by LwIP/ETH init)
extern struct netif gnetif;   // Replace with your actual LAN netif variable

void wol_print_mac(void) {
    WOL_LOG("Target MAC: %02X:%02X:%02X:%02X:%02X:%02X",
            targetMac[0], targetMac[1], targetMac[2],
            targetMac[3], targetMac[4], targetMac[5]);
}

bool wol_check_network_ready(void) {
    struct netif *netif = &gnetif;
    if (netif == NULL) {
        WOL_LOG("LAN interface not initialized!");
        return false;
    }
    if (!netif_is_up(netif) || !netif_is_link_up(netif)) {
        WOL_LOG("LAN link is down!");
        return false;
    }
    if (ip4_addr_isany_val(netif->ip_addr)) {
        WOL_LOG("LAN has no IP address!");
        return false;
    }
    char ip_str[16];
    ip4addr_ntoa_r(&netif->ip_addr, ip_str, sizeof(ip_str));
    WOL_LOG("LAN ready, IP: %s", ip_str);
    return true;
}

err_t send_wol(void) {
    struct netif *netif = &gnetif;
    struct pbuf *pkt_buf;
    err_t err;
    struct eth_hdr *ethhdr;

    if (!wol_check_network_ready()) {
        WOL_LOG("LAN not ready, cannot send WOL!");
        return ERR_IF;
    }

    WOL_LOG("Sending WOL via LAN...");
    wol_print_mac();

    // Construct magic packet (102 bytes)
    uint8_t magic_data[102];
    memset(magic_data, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(&magic_data[6 + i * 6], targetMac, 6);
    }

    // Allocate pbuf with Ethernet header
    size_t total_len = sizeof(struct eth_hdr) + sizeof(magic_data);
    pkt_buf = pbuf_alloc(PBUF_RAW, total_len, PBUF_POOL);
    if (pkt_buf == NULL) {
        WOL_LOG("Failed to allocate pbuf!");
        return ERR_MEM;
    }

    // Fill Ethernet header
    ethhdr = (struct eth_hdr *)pkt_buf->payload;
    memset(ethhdr->dest.addr, 0xFF, ETH_HWADDR_LEN);           // Broadcast
    memcpy(ethhdr->src.addr, netif->hwaddr, ETH_HWADDR_LEN);   // STM32 MAC
    ethhdr->type = htons(0x0842);   // WOL magic packet ethertype

    // Copy magic packet data
    if (pkt_buf->next != NULL) {
        pbuf_take(pkt_buf->next, magic_data, sizeof(magic_data));
    } else {
        uint8_t *payload_ptr = (uint8_t *)pkt_buf->payload + sizeof(struct eth_hdr);
        memcpy(payload_ptr, magic_data, sizeof(magic_data));
        pkt_buf->len = total_len;
        pkt_buf->tot_len = total_len;
    }

    // Send via LAN driver
    err = netif->linkoutput(netif, pkt_buf);

    pbuf_free(pkt_buf);

    if (err == ERR_OK) {
        WOL_LOG("WOL frame sent! (%d bytes via LAN)", total_len);
    } else {
        WOL_LOG("Send failed (err=%d)", err);
    }
    return err;
}