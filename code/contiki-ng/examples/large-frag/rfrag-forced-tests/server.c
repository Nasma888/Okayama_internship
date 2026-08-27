#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "sys/log.h"

#include <stdint.h>

#define LOG_MODULE "MLKEM-SERVER"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

#define MAX_PAYLOAD_LEN 1568

/*
 * Header applicatif commun au PK et au CT :
 *
 *   octets 0..1 : sequence
 *   octet  2    : indice du niveau ML-KEM
 *   octet  3    : type du message
 *   octets 4..5 : longueur totale annoncee
 *   octets 6..9 : horodatage du premier envoi PK, cote client
 *
 * Le serveur preserve seq, case_idx et l'horodatage client, mais remplace
 * le type et la longueur pour construire le CT.
 */
#define HDR_LEN 10

#define MSG_TYPE_PK 1
#define MSG_TYPE_CT 2

typedef struct {
  const char *label;
  uint16_t pk_size;
  uint16_t ct_size;
} mlkem_case_t;

static const mlkem_case_t cases[] = {
  { "MLKEM512",   800,  768 },
  { "MLKEM768",  1184, 1088 },
  { "MLKEM1024", 1568, 1568 }
};

#define NUM_CASES ((uint8_t)(sizeof(cases) / sizeof(cases[0])))

static struct simple_udp_connection udp_conn;
static uint8_t ct_buf[MAX_PAYLOAD_LEN];

static uint16_t last_rx_seq = UINT16_MAX;
static uint8_t last_rx_case_idx = 0xff;

PROCESS(udp_server_process,
        "ML-KEM server: receive public key and send ciphertext");
AUTOSTART_PROCESSES(&udp_server_process);

/*---------------------------------------------------------------------------*/
static void
write_u16_be(uint8_t *dst, uint16_t value)
{
  dst[0] = (uint8_t)(value >> 8);
  dst[1] = (uint8_t)(value & 0xff);
}
/*---------------------------------------------------------------------------*/
static uint16_t
read_u16_be(const uint8_t *src)
{
  return ((uint16_t)src[0] << 8) | (uint16_t)src[1];
}
/*---------------------------------------------------------------------------*/
static void
write_u32_be(uint8_t *dst, uint32_t value)
{
  dst[0] = (uint8_t)(value >> 24);
  dst[1] = (uint8_t)(value >> 16);
  dst[2] = (uint8_t)(value >> 8);
  dst[3] = (uint8_t)(value & 0xff);
}
/*---------------------------------------------------------------------------*/
static uint32_t
read_u32_be(const uint8_t *src)
{
  return ((uint32_t)src[0] << 24) |
         ((uint32_t)src[1] << 16) |
         ((uint32_t)src[2] << 8)  |
         (uint32_t)src[3];
}
/*---------------------------------------------------------------------------*/
/*
 * Construit le CT factice de longueur totale ct_len.
 *
 * Ce n'est pas une encapsulation cryptographique reelle. On reproduit
 * uniquement la taille reseau du ciphertext ML-KEM.
 */
static void
build_ct_payload(uint16_t seq,
                 uint8_t case_idx,
                 uint16_t ct_len,
                 uint32_t original_client_timestamp)
{
  uint16_t i;

  write_u16_be(&ct_buf[0], seq);
  ct_buf[2] = case_idx;
  ct_buf[3] = MSG_TYPE_CT;
  write_u16_be(&ct_buf[4], ct_len);
  write_u32_be(&ct_buf[6], original_client_timestamp);

  for(i = HDR_LEN; i < ct_len; i++) {
    ct_buf[i] = (uint8_t)((0xa5 + i + seq + case_idx) & 0xff);
  }
}
/*---------------------------------------------------------------------------*/
static void
udp_server_rx_callback(struct simple_udp_connection *c,
                       const uip_ipaddr_t *sender_addr,
                       uint16_t sender_port,
                       const uip_ipaddr_t *receiver_addr,
                       uint16_t receiver_port,
                       const uint8_t *data,
                       uint16_t datalen)
{
  uint16_t seq;
  uint8_t case_idx;
  uint8_t msg_type;
  uint16_t declared_len;
  uint32_t original_client_timestamp;
  uint32_t now;
  uint32_t approximate_one_way_ticks;
  uint16_t expected_pk_len;
  uint16_t ct_len;
  uint8_t is_duplicate;

  if(datalen < HDR_LEN) {
    LOG_WARN("RX paquet trop court: datalen=%u\n", datalen);
    return;
  }

  seq = read_u16_be(&data[0]);
  case_idx = data[2];
  msg_type = data[3];
  declared_len = read_u16_be(&data[4]);
  original_client_timestamp = read_u32_be(&data[6]);

  if(msg_type != MSG_TYPE_PK) {
    LOG_WARN("RX type inattendu=%u seq=%u case_idx=%u datalen=%u\n",
             msg_type, seq, case_idx, datalen);
    return;
  }

  if(case_idx >= NUM_CASES) {
    LOG_WARN("RX PK case_idx invalide=%u seq=%u datalen=%u\n",
             case_idx, seq, datalen);
    return;
  }

  expected_pk_len = cases[case_idx].pk_size;

  if(declared_len != datalen) {
    LOG_WARN("RX PK longueur incoherente case=%s seq=%u "
             "declared=%u datalen=%u\n",
             cases[case_idx].label,
             seq,
             declared_len,
             datalen);
    return;
  }

  if(datalen != expected_pk_len) {
    LOG_WARN("RX PK mauvaise taille case=%s seq=%u "
             "expected=%u received=%u\n",
             cases[case_idx].label,
             seq,
             expected_pk_len,
             datalen);
    return;
  }

  is_duplicate = (seq == last_rx_seq && case_idx == last_rx_case_idx);

  /*
   * Cette latence aller simple n'est interpretable que si les horloges
   * des deux motes sont alignees. Le RTT mesure par le client est plus
   * fiable, car il utilise une seule horloge locale.
   */
  now = (uint32_t)clock_time();
  approximate_one_way_ticks = now - original_client_timestamp;

  if(is_duplicate) {
    LOG_WARN("RX PK DUPLICATE case=%s case_idx=%u seq=%u "
             "declared=%u datalen=%u approx_one_way_ms=%lu from ",
             cases[case_idx].label,
             case_idx,
             seq,
             declared_len,
             datalen,
             (unsigned long)((approximate_one_way_ticks * 1000UL) /
                             CLOCK_SECOND));
  } else {
    last_rx_seq = seq;
    last_rx_case_idx = case_idx;

    LOG_INFO("RX PK VALID case=%s case_idx=%u seq=%u "
             "declared=%u datalen=%u approx_one_way_ticks=%lu "
             "approx_one_way_ms=%lu from ",
             cases[case_idx].label,
             case_idx,
             seq,
             declared_len,
             datalen,
             (unsigned long)approximate_one_way_ticks,
             (unsigned long)((approximate_one_way_ticks * 1000UL) /
                             CLOCK_SECOND));
  }

  LOG_INFO_6ADDR(sender_addr);
  LOG_INFO_("\n");

  /*
   * Meme pour un PK duplique, le serveur renvoie le CT :
   * le CT precedent a pu etre perdu.
   */
  ct_len = cases[case_idx].ct_size;
  build_ct_payload(seq,
                   case_idx,
                   ct_len,
                   original_client_timestamp);

  LOG_INFO("TX CT case=%s case_idx=%u seq=%u "
           "declared=%u datalen=%u to ",
           cases[case_idx].label,
           case_idx,
           seq,
           ct_len,
           ct_len);
  LOG_INFO_6ADDR(sender_addr);
  LOG_INFO_("\n");

  simple_udp_sendto(&udp_conn, ct_buf, ct_len, sender_addr);
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_server_process, ev, data)
{
  PROCESS_BEGIN();

  NETSTACK_ROUTING.root_start();

  simple_udp_register(&udp_conn,
                      UDP_SERVER_PORT,
                      NULL,
                      UDP_CLIENT_PORT,
                      udp_server_rx_callback);

  LOG_INFO("Serveur/root initialise: PK sizes=800/1184/1568, "
           "CT sizes=768/1088/1568\n");

  while(1) {
    PROCESS_WAIT_EVENT();
  }

  PROCESS_END();
}
