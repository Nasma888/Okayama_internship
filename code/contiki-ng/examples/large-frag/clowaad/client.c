// #include "contiki.h"
// #include "net/routing/routing.h"
// #include "net/netstack.h"
// #include "net/ipv6/simple-udp.h"
// #include "sys/log.h"

// #define LOG_MODULE "App"
// #define LOG_LEVEL LOG_LEVEL_INFO

// /* Délai entre deux envois : assez large pour laisser le temps au
//  * réassemblage + à l'affichage des logs avant le paquet suivant.
//  * A ajuster si SICSLOWPAN_CONF_MAXAGE ou la charge du réseau l'exigent. */
// #define SEND_INTERVAL (15 * CLOCK_SECOND)

// #define MAX_PAYLOAD_LEN 1568

// static struct simple_udp_connection udp_conn;

// /* Les 5 tailles réelles étudiées : ek/ct pour ML-KEM-512/768/1024
//  * (FIPS 203). Payload arbitraire (pas de vraie clé/ciphertext) : on
//  * étudie ici uniquement le comportement de fragmentation, pas la crypto. */
// typedef struct {
//   const char *label;
//   uint16_t    size;
// } payload_case_t;

// static const payload_case_t cases[] = {
//   { "MLKEM512_EK",     800 },
//   { "MLKEM512_CT",     768 },
//   { "MLKEM768_EK",    1184 },
//   { "MLKEM768_CT",    1088 },
//   { "MLKEM1024_EKCT", 1568 },
// };
// #define NUM_CASES (sizeof(cases) / sizeof(cases[0]))

// static uint8_t buf[MAX_PAYLOAD_LEN];

// PROCESS(udp_client_process, "UDP client - etude fragmentation ML-KEM");
// AUTOSTART_PROCESSES(&udp_client_process);
// /*---------------------------------------------------------------------------*/
// /* Payload : [2o seq][4o horodatage TX][octets de remplissage 0x00..0xFF]
//  * Le horodatage permet au serveur de calculer une latence approximative ;
//  * le numéro de séquence permet de corréler avec le Radio Logger / pcap. */
// static void
// build_payload(uint16_t seq, uint16_t len)
// {
//   uint32_t ts = (uint32_t)clock_time();
//   uint16_t i;

//   buf[0] = (uint8_t)(seq >> 8);
//   buf[1] = (uint8_t)(seq & 0xff);
//   buf[2] = (uint8_t)(ts >> 24);
//   buf[3] = (uint8_t)(ts >> 16);
//   buf[4] = (uint8_t)(ts >> 8);
//   buf[5] = (uint8_t)(ts & 0xff);

//   for(i = 6; i < len; i++) {
//     buf[i] = (uint8_t)(i & 0xff);
//   }
// }
// /*---------------------------------------------------------------------------*/
// PROCESS_THREAD(udp_client_process, ev, data)
// {
//   static struct etimer periodic_timer;
//   static uip_ipaddr_t dest_ipaddr;
//   static uint16_t case_idx = 0;
//   static uint16_t seq = 0;

//   PROCESS_BEGIN();

//   simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL,
//                        UDP_SERVER_PORT, NULL);

//   etimer_set(&periodic_timer, SEND_INTERVAL);

//   while(1) {
//     PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

//     if(NETSTACK_ROUTING.node_is_reachable() &&
//        NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {

//       if(case_idx < NUM_CASES) {
//         uint16_t len = cases[case_idx].size;

//         build_payload(seq, len);

//         LOG_INFO("TX case=%s size=%u seq=%u\n",
//                   cases[case_idx].label, len, seq);

//         simple_udp_sendto(&udp_conn, buf, len, &dest_ipaddr);

//         seq++;
//         case_idx++;
//       } else {
//         LOG_INFO("Baseline terminee : %u cas envoyes\n", (unsigned)NUM_CASES);
//         PROCESS_EXIT();
//       }
//     } else {
//       LOG_INFO("Pas encore de route vers le root (cas en attente: %s)\n",
//                 case_idx < NUM_CASES ? cases[case_idx].label : "-");
//     }

//     etimer_set(&periodic_timer, SEND_INTERVAL);
//   }

//   PROCESS_END();
// }

#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "sys/log.h"

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

/* Délai entre deux échanges (envoi ek + attente réponse ct) : assez large
 * pour laisser le temps au réassemblage + à l'affichage des logs avant
 * l'échange suivant. A ajuster si SICSLOWPAN_CONF_MAXAGE ou la charge du
 * réseau l'exigent. */
#define SEND_INTERVAL (15 * CLOCK_SECOND)

#define MAX_PAYLOAD_LEN 1568

/* Taille de l'en-tete applicatif prepende a chaque message, dans les deux
 * sens : [2o seq][1o case_idx][4o horodatage TX original] = 7 octets.
 * Le serveur RECOPIE ce header tel quel dans sa reponse ct, ce qui permet
 * au client de calculer une latence aller-retour et de correler seq/case. */
#define HDR_LEN 7

static struct simple_udp_connection udp_conn;

/* Les 3 niveaux ML-KEM etudies (FIPS 203) : tailles reelles de la cle
 * publique encapsulation (ek) envoyee par le client, et du ciphertext (ct)
 * renvoye par le serveur apres encapsulation. */
typedef struct {
  const char *label;
  uint16_t    ek_size;
  uint16_t    ct_size;
} mlkem_case_t;

static const mlkem_case_t cases[] = {
  { "MLKEM512",  800,  768 },
  { "MLKEM768",  1184, 1088 },
  { "MLKEM1024", 1568, 1568 },
};
#define NUM_CASES (sizeof(cases) / sizeof(cases[0]))

static uint8_t buf[MAX_PAYLOAD_LEN];
static uint16_t last_rx_seq = UINT16_MAX;

PROCESS(udp_client_process, "UDP client - etude fragmentation ML-KEM (ek/ct)");
AUTOSTART_PROCESSES(&udp_client_process);
/*---------------------------------------------------------------------------*/
/* Construit le payload ek : [2o seq][1o case_idx][4o horodatage TX]
 * [octets de remplissage 0x00..0xFF]. Le remplissage n'est pas une vraie
 * cle ML-KEM : on etudie ici uniquement le comportement de fragmentation
 * 6LoWPAN, pas la cryptographie elle-meme. */
static void
build_ek_payload(uint16_t seq, uint8_t case_idx, uint16_t len)
{
  uint32_t ts = (uint32_t)clock_time();
  uint16_t i;

  buf[0] = (uint8_t)(seq >> 8);
  buf[1] = (uint8_t)(seq & 0xff);
  buf[2] = case_idx;
  buf[3] = (uint8_t)(ts >> 24);
  buf[4] = (uint8_t)(ts >> 16);
  buf[5] = (uint8_t)(ts >> 8);
  buf[6] = (uint8_t)(ts & 0xff);

  for(i = HDR_LEN; i < len; i++) {
    buf[i] = (uint8_t)(i & 0xff);
  }
}
/*---------------------------------------------------------------------------*/
/* Callback de reception : traite la reponse ct envoyee par le serveur.
 * Le header (seq/case_idx/ts) est celui recopie par le serveur depuis le
 * ek d'origine, ce qui permet un calcul de latence aller-retour exact. */
static void
udp_client_rx_callback(struct simple_udp_connection *c,
                        const uip_ipaddr_t *sender_addr,
                        uint16_t sender_port,
                        const uip_ipaddr_t *receiver_addr,
                        uint16_t receiver_port,
                        const uint8_t *data,
                        uint16_t datalen)
{
  uint16_t seq;
  uint8_t case_idx;
  uint32_t ts_orig, now, rtt_latency;
  const char *label = "UNKNOWN";

  if(datalen < HDR_LEN) {
    LOG_WARN("RX ct trop court, len=%u\n", datalen);
    return;
  }

  seq      = ((uint16_t)data[0] << 8) | data[1];
  case_idx = data[2];
  ts_orig  = ((uint32_t)data[3] << 24) |
             ((uint32_t)data[4] << 16) |
             ((uint32_t)data[5] << 8)  |
             (uint32_t)data[6];

  now = (uint32_t)clock_time();
  rtt_latency = now - ts_orig;

  if(case_idx < NUM_CASES) {
    label = cases[case_idx].label;
  }

  if(seq == last_rx_seq) {
    LOG_WARN("RX CT DUPLICATE case=%s seq=%u size=%u\n", label, seq, datalen);
    return;
  }
  last_rx_seq = seq;

  LOG_INFO("RX CT case=%s seq=%u size=%u rtt_ticks=%lu (%lu ms) from ",
           label, seq, datalen,
           (unsigned long)rtt_latency,
           (unsigned long)((rtt_latency * 1000) / CLOCK_SECOND));
  LOG_INFO_6ADDR(sender_addr);
  LOG_INFO_("\n");
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer periodic_timer;
  static uip_ipaddr_t dest_ipaddr;
  static uint16_t case_idx = 0;
  static uint16_t seq = 0;

  PROCESS_BEGIN();

  simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL,
                       UDP_SERVER_PORT, udp_client_rx_callback);

  etimer_set(&periodic_timer, SEND_INTERVAL);

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

    if(NETSTACK_ROUTING.node_is_reachable() &&
       NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {

      if(case_idx < NUM_CASES) {
        uint16_t len = cases[case_idx].ek_size;

        build_ek_payload(seq, (uint8_t)case_idx, len);

        LOG_INFO("TX EK case=%s size=%u seq=%u\n",
                 cases[case_idx].label, len, seq);

        simple_udp_sendto(&udp_conn, buf, len, &dest_ipaddr);

        seq++;
        case_idx++;
      } else {
        LOG_INFO("Baseline terminee : %u echanges ek/ct envoyes\n",
                 (unsigned)NUM_CASES);
        PROCESS_EXIT();
      }
    } else {
      LOG_INFO("Pas encore de route vers le root (cas en attente: %s)\n",
               case_idx < NUM_CASES ? cases[case_idx].label : "-");
    }

    etimer_set(&periodic_timer, SEND_INTERVAL);
  }

  PROCESS_END();
}

