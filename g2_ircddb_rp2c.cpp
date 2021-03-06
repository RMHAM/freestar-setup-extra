
/*
 *   Copyright (C) 2010 by Scott Lawson KI4LKF
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful, 
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the 
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License 
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */


/* by KI4LKF */
/* 
   g2_ircddb is a dstar G2 gateway, using irc routing 
       adapted from the OpenG2 G2 gateway
   Version 2.61 or newer will use ONLY the irc mechanism of routing
     and it will NOT use any local Postgres databases or any TRUST(s)
   This file is for the ICOM RP2C repeater, not for home-brew repeaters.
*/

#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <math.h>

#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <regex.h>

#include <string>
#include <utility>
using namespace std;

#include <pthread.h>

#include <wx/hashmap.h>

#include "IRCDDB.h"

#define VERSION "Linux-g2_ircddb-rp2c-3.09"
#define OWNER_SIZE 8
#define IP_SIZE 15
#define MAXHOSTNAMELEN 64
#define CALL_SIZE 8
#define ECHO_CODE 'E'
#define STORE_VM_CODE 'S'
#define RECALL_VM_CODE 'R'
#define CLEAR_VM_CODE 'C'
#define LINK_CODE 'L'

/* configuration data */

/* Gateway callsign */
static char OWNER[OWNER_SIZE + 1];
static char owner[OWNER_SIZE + 1];
static char PACKAGE_REV[56];

static char LOCAL_IRC_IP[IP_SIZE + 1];
static bool SEND_QRGS_MAPS = false;
static char QTH[257];
static char QRG_A[257];
static char QRG_B[257];
static char QRG_C[257];

static char STATUS_FILE[FILENAME_MAX + 1];

static char silence[9] = { 0x4e,0x8d,0x32,0x88,0x26,0x1a,0x3f,0x61,0xe8 };

static char DTMF_DIR[FILENAME_MAX + 1];
static char DTMF_FILE[FILENAME_MAX + 1];
static const int MAX_DTMF_BUF = 32;
static char dtmf_chars[17] = "147*2580369#ABCD";
static int dtmf_digit;
static FILE *dtmf_fp = NULL;
static char dtmf_buf[3][MAX_DTMF_BUF + 1] = { {""}, {""}, {""} };
static int dtmf_buf_count[3] = {0, 0, 0};
static unsigned int dtmf_counter[3] = {0, 0, 0};
static int dtmf_last_frame[3] = {0, 0, 0};

static bool IRC_DEBUG = false;
static bool DTMF_DEBUG = false;
static bool REGEN_HDR = false;

/* the aprs TCP socket */
static int aprs_sock = -1;
static struct sockaddr_in aprs_addr;
static socklen_t aprs_addr_len;

/* data needed for aprs login and aprs beacon */
static struct
{
   char aprs_host[MAXHOSTNAMELEN + 1];
   int aprs_port;
   int aprs_hash;
   int aprs_interval;
   char aprs_filter[512];

   /* 0=A, 1=B, 2=C */
   char rptr[3][CALL_SIZE + 1];   /* KJ4NHF-B */
   char band[3][5];  /* 23cm ... */
   int range[3];
   double lat[3];
   double lon[3];
   char desc[3][64];
} RPTR_ID;

/* Gateway external IP and port for remote G2 routing */
static char G2_EXTERNAL_IP[IP_SIZE + 1]; /* 0.0.0.0 */
static int G2_EXTERNAL_PORT = 40000;

/* 
   Internal Gateway internal IP and port, 
     so that the local repeater modules can talk to our g2_ircddb 
*/
static char G2_INTERNAL_IP[IP_SIZE + 1]; /* Example: 172.16.0.20 */
static int G2_INTERNAL_PORT = 20000;

/* We send repeater data to our g2_link */
static char TO_G2_LINK_IP[IP_SIZE + 1];
static int TO_G2_LINK_PORT = 18997;

/* local repeater module IP and port for ICOM repeater */
static char TO_RPTR_IP[IP_SIZE + 1];  /* Example: 172.16.0.10 */
static int TO_RPTR_PORT; /* Example: 20000 */

/* QSO details go to a log file */
static bool QSO_DETAILS = false;

static bool SEND_APRS = false;

/* Recorded files are here */
static char ECHOTEST_DIR[FILENAME_MAX + 1];

/*
 How many SECONDS to wait before starting to playback the recorded file.
 1 second is enough, some repeaters require 2 seconds to re-initialize
    before they accept new streams again.
*/
static int PLAY_WAIT = 1;

/*
   How many MILLIseconds to delay when playing back each of the recorded packets
   Normal VoIP is 20 milliseconds
*/
static int PLAY_DELAY = 20;

static char IRC_DDB_HOST[512];
static int IRC_DDB_PORT = 9007;
static char IRC_PASS[512];

static int ECHOTEST_REC_TIMEOUT = 1;
static int VOICEMAIL_REC_TIMEOUT = 1;
static int FROM_REMOTE_G2_TIMEOUT = 1;
static int FROM_LOCAL_RPTR_TIMEOUT = 1;

/* local repeater modules being recorded */
/* This is for echotest */
static struct
{
   time_t last_time;
   unsigned char streamid[2];
   int fd;
   char file[FILENAME_MAX + 1];
} recd[3];

/* this is for vm */
static struct
{
   time_t last_time;
   unsigned char streamid[2];
   int fd;
   char file[FILENAME_MAX + 1];
} vm[3];
static unsigned char recbuf[100]; /* 56 or 27, max is 56 */

/* the streamids going to remote Gateways from each local module */
static struct
{
   unsigned char streamid[2];
   struct sockaddr_in toDst4;
   time_t last_time;
} to_remote_g2[3]; /* 0=A, 1=B, 2=C */

/* input from remote G2 gateway */
static int g2_sock = -1;
static unsigned char readBuffer2[2000]; /* 56 or 27, max is 56 */
static struct sockaddr_in fromDst4;

/* 
   Incoming data from remote systems 
   must be fed into our ICOM RP2C.
*/
static struct
{
   /* help with header re-generation */
   unsigned char saved_hdr[58]; /* repeater format */
   uint32_t saved_adr;

   unsigned char streamid[2];
   uint32_t adr; 
   time_t last_time;
} toRptr_rp2c[3]; /* 0=A, 1=B, 2=C */
static struct sockaddr_in toRptr;
static unsigned short G2_COUNTER_OUT = 0;
static unsigned short OLD_REPLY_SEQ = 0;
static unsigned short NEW_REPLY_SEQ = 0;
static unsigned short TEMP_SEQ = 0;

/* input from our own RP2C */
static int srv_sock = -1;
static unsigned char readBuffer[2000]; /* 58 or 29 or 32, max is 58 */
static struct sockaddr_in fromRptr;

static volatile bool keep_running = true;

static time_t wd_timer = 0;

/* send packets to g2_link */
static struct sockaddr_in plug;

/* for talking with the irc server */
static CIRCDDB *ii;

enum aprs_level
{
   al_none,
   al_$1,
   al_$2,
   al_c1,
   al_r1,
   al_c2,
   al_csum1,
   al_csum2,
   al_csum3,
   al_csum4,
   al_data,
   al_end
};

enum slow_level
{
   sl_first,
   sl_second
};

static struct
{
   aprs_level al;
   unsigned char data[300];
   unsigned int len;
   unsigned char buf[6];
   slow_level sl;
   bool is_sent;
} aprs_pack[3];

/* lock down a stream per band */
static struct
{
   unsigned char streamID[2];
   time_t last_time;
} aprs_streamID[3];

/* text coming from local repeater bands */
static struct
{
   unsigned char streamID[2];
   unsigned char flags[3];
   char lh_mycall[CALL_SIZE + 1];
   char lh_sfx[5];
   char lh_yrcall[CALL_SIZE + 1];
   char lh_rpt1[CALL_SIZE + 1];
   char lh_rpt2[CALL_SIZE + 1];
   char txt[64];   /* Only 20 are used */
   time_t last_time;
   unsigned short txt_cnt;
   bool txt_stats_sent;

   char dest_rptr[CALL_SIZE + 1];

   /* try to process GPS mode: GPRMC and ID */
   char temp_line[256];
   unsigned short temp_line_cnt;
   char gprmc[256];
   char gpid[256];
   bool is_gps_sent;
   time_t gps_last_time;

   int num_dv_frames;
   int num_dv_silent_frames;
   int num_bit_errors;

} band_txt[3]; /* 0=A, 1=B, 2=C */

/* Used to validate MYCALL input */
static regex_t preg;

/* 
   CACHE used to cache users, repeaters, 
    gateways, IP numbers coming from the irc server
*/
WX_DECLARE_STRING_HASH_MAP(wxString, user2rptr_type);
static user2rptr_type user2rptr_map;

WX_DECLARE_STRING_HASH_MAP(wxString, rptr2gwy_type);
static rptr2gwy_type rptr2gwy_map;

WX_DECLARE_STRING_HASH_MAP(wxString, gwy2ip_type);
static gwy2ip_type gwy2ip_map;

static pthread_mutex_t irc_data_mutex = PTHREAD_MUTEX_INITIALIZER;

static unsigned short crc_tabccitt[256] =
{
   0x0000,0x1189,0x2312,0x329b,0x4624,0x57ad,0x6536,0x74bf,
   0x8c48,0x9dc1,0xaf5a,0xbed3,0xca6c,0xdbe5,0xe97e,0xf8f7,
   0x1081,0x0108,0x3393,0x221a,0x56a5,0x472c,0x75b7,0x643e,
   0x9cc9,0x8d40,0xbfdb,0xae52,0xdaed,0xcb64,0xf9ff,0xe876,
   0x2102,0x308b,0x0210,0x1399,0x6726,0x76af,0x4434,0x55bd,
   0xad4a,0xbcc3,0x8e58,0x9fd1,0xeb6e,0xfae7,0xc87c,0xd9f5,
   0x3183,0x200a,0x1291,0x0318,0x77a7,0x662e,0x54b5,0x453c,
   0xbdcb,0xac42,0x9ed9,0x8f50,0xfbef,0xea66,0xd8fd,0xc974,
   0x4204,0x538d,0x6116,0x709f,0x0420,0x15a9,0x2732,0x36bb,
   0xce4c,0xdfc5,0xed5e,0xfcd7,0x8868,0x99e1,0xab7a,0xbaf3,
   0x5285,0x430c,0x7197,0x601e,0x14a1,0x0528,0x37b3,0x263a,
   0xdecd,0xcf44,0xfddf,0xec56,0x98e9,0x8960,0xbbfb,0xaa72,
   0x6306,0x728f,0x4014,0x519d,0x2522,0x34ab,0x0630,0x17b9,
   0xef4e,0xfec7,0xcc5c,0xddd5,0xa96a,0xb8e3,0x8a78,0x9bf1,
   0x7387,0x620e,0x5095,0x411c,0x35a3,0x242a,0x16b1,0x0738,
   0xffcf,0xee46,0xdcdd,0xcd54,0xb9eb,0xa862,0x9af9,0x8b70,
   0x8408,0x9581,0xa71a,0xb693,0xc22c,0xd3a5,0xe13e,0xf0b7,
   0x0840,0x19c9,0x2b52,0x3adb,0x4e64,0x5fed,0x6d76,0x7cff,
   0x9489,0x8500,0xb79b,0xa612,0xd2ad,0xc324,0xf1bf,0xe036,
   0x18c1,0x0948,0x3bd3,0x2a5a,0x5ee5,0x4f6c,0x7df7,0x6c7e,
   0xa50a,0xb483,0x8618,0x9791,0xe32e,0xf2a7,0xc03c,0xd1b5,
   0x2942,0x38cb,0x0a50,0x1bd9,0x6f66,0x7eef,0x4c74,0x5dfd,
   0xb58b,0xa402,0x9699,0x8710,0xf3af,0xe226,0xd0bd,0xc134,
   0x39c3,0x284a,0x1ad1,0x0b58,0x7fe7,0x6e6e,0x5cf5,0x4d7c,
   0xc60c,0xd785,0xe51e,0xf497,0x8028,0x91a1,0xa33a,0xb2b3,
   0x4a44,0x5bcd,0x6956,0x78df,0x0c60,0x1de9,0x2f72,0x3efb,
   0xd68d,0xc704,0xf59f,0xe416,0x90a9,0x8120,0xb3bb,0xa232,
   0x5ac5,0x4b4c,0x79d7,0x685e,0x1ce1,0x0d68,0x3ff3,0x2e7a,
   0xe70e,0xf687,0xc41c,0xd595,0xa12a,0xb0a3,0x8238,0x93b1,
   0x6b46,0x7acf,0x4854,0x59dd,0x2d62,0x3ceb,0x0e70,0x1ff9,
   0xf78f,0xe606,0xd49d,0xc514,0xb1ab,0xa022,0x92b9,0x8330,
   0x7bc7,0x6a4e,0x58d5,0x495c,0x3de3,0x2c6a,0x1ef1,0x0f78
};

static int g2_open();
static int srv_open();
static void calcPFCS(unsigned char *packet, int len);
static void *get_irc_data(void *arg);
static int get_yrcall_rptr_from_cache(char *call, char *arearp_cs, char *zonerp_cs,
                           char *mod, char *ip, char RoU);
static bool get_yrcall_rptr(char *call, char *arearp_cs, char *zonerp_cs, 
                           char *mod, char *ip, char RoU);
static void traceit(const char *fmt,...);
static int read_config(char *);
static void runit();
static void sigCatch(int signum);
void *echotest(void *arg);
static void compute_aprs_hash();
static void *send_aprs_beacon(void *arg);

/* aprs functions, borrowed from my retired IRLP node 4201 */
static bool aprs_write_data(short int rptr_idx, unsigned char *data);
static void aprs_sync_it(short int rptr_idx);
static void aprs_reset(short int rptr_idx);
static unsigned int aprs_get_data(short int rptr_idx, unsigned char *data, unsigned int len);
static void aprs_init();
static void aprs_open();
static bool aprs_add_data(short int rptr_idx, unsigned char *data);
static bool aprs_check_data(short int rptr_idx);
static unsigned int aprs_calc_crc(unsigned char* buf, unsigned int len);
static void aprs_select_band(short int rptr_idx, unsigned char *streamID);
static void aprs_process_text(unsigned char *streamID,
                              unsigned char seq,
                              unsigned char *buf,
                              unsigned int len);

static void gps_send(short int rptr_idx);
static bool verify_gps_csum(char *gps_text, char *csum_text);
static void build_aprs_from_gps_and_send(short int rptr_idx);
static ssize_t writen(char *buffer, size_t n);

static void qrgs_and_maps();

static bool resolve_rmt(char *name, int type, struct sockaddr_in *addr);

static void set_dest_rptr(int mod_ndx, char *dest_rptr);

extern void dstar_dv_init();
extern int dstar_dv_decode(const unsigned char *d, int data[3]);

static bool resolve_rmt(char *name, int type, struct sockaddr_in *addr)
{
   struct addrinfo hints;
   struct addrinfo *res;
   struct addrinfo *rp;
   int rc = 0;
   bool found = false;

   memset(&hints, 0x00, sizeof(struct addrinfo));
   hints.ai_family = AF_INET;
   hints.ai_socktype = type;

   rc = getaddrinfo(name, NULL, &hints, &res);
   if (rc != 0)
   {
      traceit("getaddrinfo return error code %d for [%s]\n", rc, name);
      return false;
   }

   for (rp = res; rp != NULL; rp = rp->ai_next)
   {
      if ((rp->ai_family == AF_INET) &&
          (rp->ai_socktype == type))
      {
         memcpy(addr, rp->ai_addr, sizeof(struct sockaddr_in));
         found = true;
         break;
      }
   }
   freeaddrinfo(res);
   return found;
}

static void set_dest_rptr(int mod_ndx, char *dest_rptr)
{
   FILE *statusfp = NULL;
   char statusbuf[1024];
   char *status_local_mod = NULL;
   char *status_remote_stm = NULL;
   char *status_remote_mod = NULL;
   const char *delim = ",";
   char *saveptr = NULL;
   char *p = NULL;

   statusfp = fopen(STATUS_FILE, "r");
   if (statusfp)
   {
      setvbuf(statusfp, (char *)NULL, _IOLBF, 0);

      while (fgets(statusbuf, 1020, statusfp) != NULL)
      {
         p = strchr(statusbuf, '\r');
         if (p)
            *p = '\0';
         p = strchr(statusbuf, '\n');
         if (p)
            *p = '\0';

         status_local_mod = strtok_r(statusbuf, delim, &saveptr);
         status_remote_stm = strtok_r(NULL, delim, &saveptr);
         status_remote_mod = strtok_r(NULL, delim, &saveptr);

         if (!status_local_mod || !status_remote_stm || !status_remote_mod)
            continue;

         if ( ((*status_local_mod == 'A') && (mod_ndx == 0))  ||
              ((*status_local_mod == 'B') && (mod_ndx == 1))  ||
              ((*status_local_mod == 'C') && (mod_ndx == 2)) )
         {
            strncpy(dest_rptr, status_remote_stm, CALL_SIZE);
            dest_rptr[7] = *status_remote_mod;
            dest_rptr[CALL_SIZE] = '\0';
            break;
         }
      }
      fclose(statusfp);
   }

   return;
}

/* compute checksum */
static void calcPFCS(unsigned char *packet, int len)
{
   unsigned short crc_dstar_ffff = 0xffff;
   unsigned short tmp, short_c;
   short int i;
   short int low;
   short int high;

   if (len == 56)
   {
      low = 15;
      high = 54;
   }
   else
   if (len == 58)
   {
      low = 17;
      high = 56;
   }
   else
      return;

   for (i = low; i < high ; i++)
   {
      short_c = 0x00ff & (unsigned short)packet[i];
      tmp = (crc_dstar_ffff & 0x00ff) ^ short_c;
      crc_dstar_ffff = (crc_dstar_ffff >> 8) ^ crc_tabccitt[tmp];
   }
   crc_dstar_ffff =  ~crc_dstar_ffff;
   tmp = crc_dstar_ffff;

   if (len == 56)
   {
      packet[54] = (unsigned char)(crc_dstar_ffff & 0xff);
      packet[55] = (unsigned char)((tmp >> 8) & 0xff);
   }
   else
   {
      packet[56] = (unsigned char)(crc_dstar_ffff & 0xff);
      packet[57] = (unsigned char)((tmp >> 8) & 0xff);
   }

   return;   
}

/* log the event */
static void traceit(const char *fmt,...)
{
   time_t ltime;
   struct tm tm;
   const short BFSZ = 512;
   char buf[BFSZ];

   time(&ltime);
   localtime_r(&ltime,&tm);

   snprintf(buf,BFSZ - 1,"%02d%02d%02d at %d:%02d:%02d:",
            tm.tm_mon+1,tm.tm_mday,tm.tm_year % 100,
            tm.tm_hour,tm.tm_min,tm.tm_sec);

   va_list args;
   va_start(args,fmt);
   vsnprintf(buf + strlen(buf), BFSZ - strlen(buf) -1, fmt, args);
   va_end(args);

   fprintf(stdout, "%s", buf);
   return;

}

/* process configuration file */
static int read_config(char *cfgFile)
{
   short int valid_params = 40;
   short int params = 0;
   unsigned short i;

   FILE *cnf = NULL;
   char inbuf[1024];
   char *p = NULL;
   char *ptr;
   char *tok = NULL;
   const char *delim = ",";

   cnf = fopen(cfgFile, "r");
   if (!cnf)
   {
      traceit("Failed to open file %s\n", cfgFile);
      return 1;
   }

   traceit("Reading file %s\n", cfgFile);
   while (fgets(inbuf, 1020, cnf) != NULL)
   {
      if (strchr(inbuf, '#'))
         continue;

      p = strchr(inbuf, '\r');
      if (p)
         *p = '\0';
      p = strchr(inbuf, '\n');
      if (p)
         *p = '\0';

      p = strchr(inbuf, '=');
      if (!p)
         continue;
      *p = '\0';

      if (strcmp(inbuf,"OWNER") == 0)
      {
          memset(OWNER,' ', sizeof(OWNER));
          OWNER[OWNER_SIZE] = '\0';
          strcpy(owner, OWNER);

          ptr = strchr(p + 1, ' ');
          if (ptr)
             *ptr = '\0';
          
          if ((strlen(p + 1) < 3) || (strlen(p + 1) > (OWNER_SIZE - 2)))
             traceit("OWNER value [%s] invalid\n", p + 1);
          else
          {
             memcpy(OWNER, p + 1, strlen(p + 1)); 

             for (i = 0; i < strlen(OWNER); i++)
                OWNER[i] = toupper(OWNER[i]);

             traceit("OWNER=[%s]\n",OWNER);

             for (i = 0; i < strlen(OWNER); i++)
             {
                if (OWNER[i] == ' ')
                { 
                   owner[i] = '\0';
                   break;
                }
                owner[i] = tolower(OWNER[i]);
             }
             params ++;
          }
      }
      else
      if (strcmp(inbuf,"STATUS_FILE") == 0)
      {
         memset(STATUS_FILE, '\0', sizeof(STATUS_FILE));
         strncpy(STATUS_FILE, p + 1,FILENAME_MAX);
         traceit("STATUS_FILE=[%s]\n",STATUS_FILE);
         params ++;
      }
      else
      if (strcmp(inbuf,"PACKAGE_REV") == 0)
      {
         memset(PACKAGE_REV, 0, sizeof(PACKAGE_REV));
         strncpy(PACKAGE_REV, p + 1, 55);
         traceit("PACKAGE_REV=[%s]\n", PACKAGE_REV);
         params ++;
      }
      else
      if (strcmp(inbuf,"LOCAL_IRC_IP") == 0)
      {
         ptr = strchr(p + 1, ' ');
         if (ptr)
            *ptr = '\0';

         if (strlen(p + 1) < 1)
            traceit("LOCAL_IRC_IP value [%s] invalid\n", p + 1);
         else
         {
            strncpy(LOCAL_IRC_IP, p + 1, IP_SIZE);
            LOCAL_IRC_IP[IP_SIZE] = '\0';
            traceit("LOCAL_IRC_IP=[%s]\n", LOCAL_IRC_IP);
            params ++;
         }
      }
      else
      if (strcmp(inbuf,"SEND_QRGS_MAPS") == 0)
      {
         if (*(p + 1) == 'Y')
            SEND_QRGS_MAPS = true;
         else
            SEND_QRGS_MAPS = false;
         traceit("SEND_QRGS_MAPS=[%c]\n", *(p + 1));
         params ++;
      }
      else
      if (strcmp(inbuf,"QTH") == 0)
      {
         memset(QTH, '\0', sizeof(QTH));
         strncpy(QTH, p + 1, 256);
         traceit("QTH=[%s]\n", QTH);
         params++;
      }
      else
      if (strcmp(inbuf,"QRG_A") == 0)
      {
         memset(QRG_A, '\0', sizeof(QRG_A));
         strncpy(QRG_A, p + 1, 256);
         traceit("QRG_A=[%s]\n", QRG_A);
         params++;
      }
      else
      if (strcmp(inbuf,"QRG_B") == 0)
      {
         memset(QRG_B, '\0', sizeof(QRG_B));
         strncpy(QRG_B, p + 1, 256);
         traceit("QRG_B=[%s]\n", QRG_B);
         params++;
      }
      else
      if (strcmp(inbuf,"QRG_C") == 0)
      {
         memset(QRG_C, '\0', sizeof(QRG_C));
         strncpy(QRG_C, p + 1, 256);
         traceit("QRG_C=[%s]\n", QRG_C);
         params++;
      }
      else
      if (strcmp(inbuf,"APRS_HOST") == 0)
      {
         memset(RPTR_ID.aprs_host, '\0', sizeof(RPTR_ID.aprs_host));
         strncpy(RPTR_ID.aprs_host, p + 1, MAXHOSTNAMELEN);
         traceit("APRS_HOST=[%s]\n", RPTR_ID.aprs_host);
         params++;
      }
      else
      if (strcmp(inbuf,"APRS_PORT") == 0)
      {
         RPTR_ID.aprs_port = atoi(p + 1);
         traceit("APRS_PORT=[%d]\n", RPTR_ID.aprs_port);
         params++;
      }
      else
      if (strcmp(inbuf,"APRS_INTERVAL") == 0)
      {
         RPTR_ID.aprs_interval = atoi(p + 1);
         traceit("APRS_INTERVAL=[%d]\n", RPTR_ID.aprs_interval);
         if (RPTR_ID.aprs_interval < 40)
         {
            RPTR_ID.aprs_interval = 40;
            traceit("APRS_INTERVAL is low number, re-setting to 40\n");
         }
         params++;
      }
      else
      if (strcmp(inbuf,"APRS_FILTER") == 0)
      {
         memset(RPTR_ID.aprs_filter, '\0', sizeof(RPTR_ID.aprs_filter));
         strncpy(RPTR_ID.aprs_filter, p + 1, 511);
         traceit("APRS_filter=[%s]\n", RPTR_ID.aprs_filter);
         params++;
      }
      else
      if (strcmp(inbuf, "RPTR_ID_A") == 0)
      {
         traceit("RPTR_ID_A=[%s]\n", p + 1);

         tok = strtok(p + 1, delim);
         if (tok)
         {
            RPTR_ID.lat[0] = atof(tok);
            tok = strtok(NULL, delim);
            if (tok)
            {
               RPTR_ID.lon[0] = atof(tok);
               tok = strtok(NULL,delim);
               if (tok)
               {
                  RPTR_ID.range[0] = atoi(tok);
                  tok = strtok(NULL, delim);
                  if (tok)
                  {
                     memset(RPTR_ID.desc[0], '\0', 64);
                     strncpy(RPTR_ID.desc[0], tok, 63);
                     params++;

                     traceit("ModA: lat=[%lf], lon=[%lf], range=[%d], descr=[%s]\n",
                             RPTR_ID.lat[0], RPTR_ID.lon[0],
                             RPTR_ID.range[0], RPTR_ID.desc[0]);
                  }
                  else
                     traceit("ModA:Invalid value for description\n");
               }
               else
                  traceit("ModA:Invalid value for range\n");
            }
            else
               traceit("ModA:Invalid values for longitude\n");
         }
         else
         {
            params++;
            traceit("ModA aprs parameters NOT defined...ok\n");
         }
      }
      else
      if (strcmp(inbuf, "RPTR_ID_B") == 0)
      {
         traceit("RPTR_ID_B=[%s]\n", p + 1);

         tok = strtok(p + 1, delim);
         if (tok)
         {
            RPTR_ID.lat[1] = atof(tok);
            tok = strtok(NULL, delim);
            if (tok)
            {
               RPTR_ID.lon[1] = atof(tok);
               tok = strtok(NULL, delim);
               if (tok)
               {
                  RPTR_ID.range[1] = atoi(tok);
                  tok = strtok(NULL, delim);
                  if (tok)
                  {
                     memset(RPTR_ID.desc[1], '\0', 64);
                     strncpy(RPTR_ID.desc[1], tok, 63);
                     params++;

                     traceit("ModB: lat=[%lf], lon=[%lf], range=[%d], descr=[%s]\n",
                              RPTR_ID.lat[1], RPTR_ID.lon[1],
                              RPTR_ID.range[1], RPTR_ID.desc[1]);
                  }
                  else
                     traceit("ModB:Invalid value for description\n");
               }
               else
                  traceit("ModB:Invalid value for range\n");
            }
            else
               traceit("ModB:Invalid values for longitude\n");
         }
         else
         {
            params++;
            traceit("ModB aprs parameters NOT defined...ok\n");
         }
      }
      else
      if (strcmp(inbuf, "RPTR_ID_C") == 0)
      {
         traceit("RPTR_ID_C=[%s]\n", p + 1);

         tok = strtok(p + 1, delim);
         if (tok)
         {
            RPTR_ID.lat[2] = atof(tok);
            tok = strtok(NULL, delim);
            if (tok)
            {
               RPTR_ID.lon[2] = atof(tok);
               tok = strtok(NULL, delim);
               if (tok)
               {
                  RPTR_ID.range[2] = atoi(tok);
                  tok = strtok(NULL, delim);
                  if (tok)
                  {
                     memset(RPTR_ID.desc[2], '\0', 64);
                     strncpy(RPTR_ID.desc[2], tok, 63);
                     params++;

                     traceit("ModC: lat=[%lf], lon=[%lf], range=[%d], descr=[%s]\n",
                              RPTR_ID.lat[2], RPTR_ID.lon[2],
                              RPTR_ID.range[2], RPTR_ID.desc[2]);
                  }
                  else
                     traceit("ModC:Invalid value for description\n");
               }
               else
                  traceit("ModC:Invalid value for range\n");
            }
            else
               traceit("ModC:Invalid values for longitude\n");
         }
         else
         {
            params++;
            traceit("ModC aprs parameters NOT defined...ok\n");
         }
      }
      else
      if (strcmp(inbuf,"G2_EXTERNAL_IP") == 0)
      {
         ptr = strchr(p + 1, ' ');
         if (ptr)
            *ptr = '\0'; 

         if (strlen(p + 1) < 1) 
            traceit("G2_EXTERNAL_IP value [%s] invalid\n", p + 1); 
         else    
         {       
            strncpy(G2_EXTERNAL_IP, p + 1, IP_SIZE);
            G2_EXTERNAL_IP[IP_SIZE] = '\0';
            traceit("G2_EXTERNAL_IP=[%s]\n", G2_EXTERNAL_IP);
            params ++;
         }
      }
      else
      if (strcmp(inbuf,"G2_EXTERNAL_PORT") == 0)
      {
         G2_EXTERNAL_PORT = atoi(p + 1);
         traceit("G2_EXTERNAL_PORT=[%d]\n",G2_EXTERNAL_PORT);
         params ++;
      }
      else    
      if (strcmp(inbuf,"G2_INTERNAL_IP") == 0)
      {
         ptr = strchr(p + 1, ' ');
         if (ptr)
            *ptr = '\0'; 

         if (strlen(p + 1) < 1) 
            traceit("G2_INTERNAL_IP value [%s] invalid\n", p + 1); 
         else
         {
            strncpy(G2_INTERNAL_IP, p + 1, IP_SIZE);
            G2_INTERNAL_IP[IP_SIZE] = '\0'; 
            traceit("G2_INTERNAL_IP=[%s]\n", G2_INTERNAL_IP);
            params ++;
         }
      }
      else    
      if (strcmp(inbuf,"G2_INTERNAL_PORT") == 0)
      {
         G2_INTERNAL_PORT = atoi(p + 1);
         traceit("G2_INTERNAL_PORT=[%d]\n",G2_INTERNAL_PORT);
         params ++;
      }
      else
      if (strcmp(inbuf,"TO_G2_LINK_IP") == 0)
      {
         ptr = strchr(p + 1, ' ');
         if (ptr)
            *ptr = '\0';

         if (strlen(p + 1) < 1)
            traceit("TO_G2_LINK_IP value [%s] invalid\n", p + 1);
         else
         {
            strncpy(TO_G2_LINK_IP, p + 1, IP_SIZE);
            TO_G2_LINK_IP[IP_SIZE] = '\0';
            traceit("TO_G2_LINK_IP=[%s]\n", TO_G2_LINK_IP);
            params ++;
         }
      }
      else
      if (strcmp(inbuf,"TO_G2_LINK_PORT") == 0)
      {
         TO_G2_LINK_PORT = atoi(p + 1);
         traceit("TO_G2_LINK_PORT=[%d]\n",TO_G2_LINK_PORT);
         params ++;
      }
      else
      if (strcmp(inbuf,"TO_RPTR_IP") == 0)
      {
         ptr = strchr(p + 1, ' ');
         if (ptr)
            *ptr = '\0';

         if (strlen(p + 1) < 1)
            traceit("TO_RPTR_IP value [%s] invalid\n", p + 1);
         else
         {
            strncpy(TO_RPTR_IP, p + 1, IP_SIZE);
            TO_RPTR_IP[IP_SIZE] = '\0';
            traceit("TO_RPTR_IP=[%s]\n", TO_RPTR_IP);
            params ++;
         }
      }
      else
      if (strcmp(inbuf,"TO_RPTR_PORT") == 0)
      {
         TO_RPTR_PORT = atoi(p + 1);
         traceit("TO_RPTR_PORT=[%d]\n",TO_RPTR_PORT);
         params ++;
      }
      else
      if (strcmp(inbuf,"QSO_DETAILS") == 0)
      {
         if (*(p + 1) == 'Y')
            QSO_DETAILS = true;
         else
            QSO_DETAILS = false;
         traceit("QSO_DETAILS=[%c]\n", *(p + 1));
         params ++;
      }
      else
      if (strcmp(inbuf,"IRC_DEBUG") == 0)
      {
         if (*(p + 1) == 'Y')
            IRC_DEBUG = true;
         else
            IRC_DEBUG = false;
         traceit("IRC_DEBUG=[%c]\n", *(p + 1));
         params ++;
      }
      else
      if (strcmp(inbuf,"DTMF_DEBUG") == 0)
      {
         if (*(p + 1) == 'Y')
            DTMF_DEBUG = true;
         else
            DTMF_DEBUG = false;
         traceit("DTMF_DEBUG=[%c]\n", *(p + 1));
         params ++;
      }
      else
      if (strcmp(inbuf,"REGEN_HDR") == 0)
      {
         if (*(p + 1) == 'Y')
            REGEN_HDR = true;
         else
            REGEN_HDR = false;
         traceit("REGEN_HDR=[%c]\n", *(p + 1));
         params ++;
      }
      else
      if (strcmp(inbuf,"SEND_APRS") == 0)
      {
         if (*(p + 1) == 'Y')
            SEND_APRS = true;
         else
            SEND_APRS = false;
         traceit("SEND_APRS=[%c]\n", *(p + 1));
         params ++;
      }
      else
      if (strcmp(inbuf,"ECHOTEST_DIR") == 0)
      {
         memset(ECHOTEST_DIR, '\0', sizeof(ECHOTEST_DIR));
         strncpy(ECHOTEST_DIR, p + 1, FILENAME_MAX);
         traceit("ECHOTEST_DIR=[%s]\n", ECHOTEST_DIR);
         params ++;
      }
      else
      if (strcmp(inbuf, "PLAY_WAIT") == 0)
      {
         PLAY_WAIT = atoi(p + 1);
         if ((PLAY_WAIT > 10) || (PLAY_WAIT < 1))
             PLAY_WAIT = 1;
         traceit("PLAY_WAIT=[%d]\n", PLAY_WAIT);
         params ++;
      }
      else
      if (strcmp(inbuf, "PLAY_DELAY") == 0)
      {
         PLAY_DELAY = atoi(p + 1);
         if ((PLAY_DELAY > 50) || (PLAY_DELAY < 10))
             PLAY_DELAY = 20;
         traceit("PLAY_DELAY=[%d]\n", PLAY_DELAY);
         params ++;
      }
      else
      if (strcmp(inbuf,"ECHOTEST_REC_TIMEOUT") == 0)
      {
         ECHOTEST_REC_TIMEOUT = atoi(p + 1);
         traceit("ECHOTEST_REC_TIMEOUT=[%d]\n", ECHOTEST_REC_TIMEOUT);
         params ++;
      }
      else
      if (strcmp(inbuf,"VOICEMAIL_REC_TIMEOUT") == 0)
      {
         VOICEMAIL_REC_TIMEOUT = atoi(p + 1);
         traceit("VOICEMAIL_REC_TIMEOUT=[%d]\n", VOICEMAIL_REC_TIMEOUT);
         params ++;
      }
      else
      if (strcmp(inbuf,"FROM_REMOTE_G2_TIMEOUT") == 0)
      {
         FROM_REMOTE_G2_TIMEOUT = atoi(p + 1);
         traceit("FROM_REMOTE_G2_TIMEOUT=[%d]\n", FROM_REMOTE_G2_TIMEOUT);
         params ++;
      }
      else
      if (strcmp(inbuf,"FROM_LOCAL_RPTR_TIMEOUT") == 0)
      {
         FROM_LOCAL_RPTR_TIMEOUT = atoi(p + 1);
         traceit("FROM_LOCAL_RPTR_TIMEOUT=[%d]\n", FROM_LOCAL_RPTR_TIMEOUT);
         params ++;
      }
      else
      if (strcmp(inbuf,"IRC_DDB_HOST") == 0)
      {
         ptr = strchr(p + 1, ' ');
         if (ptr)
            *ptr = '\0';

         if (strlen(p + 1) < 1)
            traceit("IRC_DDB_HOST value [%s] invalid\n", p + 1);
         else
         {
            memset(IRC_DDB_HOST, '\0', sizeof(IRC_DDB_HOST));
            strncpy(IRC_DDB_HOST, p + 1, sizeof(IRC_DDB_HOST) - 1);
            traceit("IRC_DDB_HOST=[%s]\n", IRC_DDB_HOST);
            params ++;
         }
      }
      else
      if (strcmp(inbuf,"IRC_DDB_PORT") == 0)
      {
         IRC_DDB_PORT = atoi(p + 1);
         traceit("IRC_DDB_PORT=[%d]\n",IRC_DDB_PORT);
         params ++;
      }
      else
      if (strcmp(inbuf,"IRC_PASS") == 0)
      {
         memset(IRC_PASS, '\0', sizeof(IRC_PASS));
         strncpy(IRC_PASS, p + 1, sizeof(IRC_PASS) - 1);
         params ++;
      }
      else
      if (strcmp(inbuf,"DTMF_DIR") == 0)
      {
         memset(DTMF_DIR, '\0', sizeof(DTMF_DIR));
         strncpy(DTMF_DIR, p + 1, FILENAME_MAX);
         traceit("DTMF_DIR=[%s]\n", DTMF_DIR);
         params ++;
      }
   }
   fclose(cnf);

   if (params != valid_params)
   {
      traceit("Configuration file %s invalid\n",cfgFile);
      return 1;
   } 
   return 0;
}

/* Create the 40000 external port */
static int g2_open()
{
   struct sockaddr_in sin;

   g2_sock = socket(PF_INET,SOCK_DGRAM,0);
   if (g2_sock == -1)
   {
      traceit("Failed to create g2 socket,errno=%d\n",errno);
      return 1;
   }
   fcntl(g2_sock,F_SETFL,O_NONBLOCK);

   memset(&sin,0,sizeof(struct sockaddr_in));
   sin.sin_family = AF_INET;
   sin.sin_port = htons(G2_EXTERNAL_PORT);
   sin.sin_addr.s_addr = inet_addr(G2_EXTERNAL_IP);

   if (bind(g2_sock,(struct sockaddr *)&sin,sizeof(struct sockaddr_in)) != 0)
   {
      traceit("Failed to bind g2 socket on port %d, errno=%d\n",G2_EXTERNAL_PORT,errno);
      close(g2_sock);
      g2_sock = -1;
      return 1;
   }
   return 0;
}

/* Create the 20000 internal port */
static int srv_open()
{
   struct sockaddr_in sin;
   
   srv_sock = socket(PF_INET,SOCK_DGRAM,0);
   if (srv_sock == -1)
   {
      traceit("Failed to create srv socket,errno=%d\n",errno);
      return 1;
   }
   fcntl(srv_sock,F_SETFL,O_NONBLOCK);

   memset(&sin,0,sizeof(struct sockaddr_in));
   sin.sin_family = AF_INET;
   sin.sin_port = htons(G2_INTERNAL_PORT);
   sin.sin_addr.s_addr = inet_addr(G2_INTERNAL_IP);
   
   if (bind(srv_sock,(struct sockaddr *)&sin,sizeof(struct sockaddr_in)) != 0)
   {
      traceit("Failed to bind srv socket on port %d, errno=%d\n",
              G2_INTERNAL_PORT,errno);
      close(srv_sock); 
      srv_sock = -1;
      return 1;
   }

   return 0;
}

/* receive data from the irc server and save it */
static void *get_irc_data(void *arg)
{
   struct timespec req;

   wxString user;
   wxString rptr;
   wxString gateway;
   wxString ipaddr;
   DSTAR_PROTOCOL proto;
   IRCDDB_RESPONSE_TYPE type;
   int rc = 0;
   struct sigaction act;
   short threshold = 0;
   short THRESHOLD_MAX = 100;
   short last_status = 0;

   arg = arg;

   act.sa_handler = sigCatch;
   sigemptyset(&act.sa_mask);
   act.sa_flags = SA_RESTART;
   if (sigaction(SIGTERM, &act, 0) != 0)
   {
      traceit("sigaction-TERM failed, error=%d\n", errno);
      traceit("get_irc_data thread exiting...\n");
      keep_running = false;
      pthread_exit(NULL);
   }
   if (sigaction(SIGINT, &act, 0) != 0)
   {
      traceit("sigaction-INT failed, error=%d\n", errno);
      traceit("get_irc_data thread exiting...\n");
      keep_running = false;
      pthread_exit(NULL);
   }
   if (sigaction(SIGPIPE, &act, 0) != 0)
   {
      traceit("sigaction-PIPE failed, error=%d\n", errno);
      traceit("get_irc_data thread exiting...\n");
      keep_running = false;
      pthread_exit(NULL);
   }

   while (keep_running)
   {
      threshold ++;
      if (threshold >= THRESHOLD_MAX)
      {
         rc = ii->getConnectionState();
         if ((rc == 0) || (rc == 10))
         {
            if (last_status != 0)
            {
               traceit("irc status=%d, probable disconnect...\n", rc);
               last_status = 0;
            }
         }
         else
         if (rc == 7)
         {
            if (last_status != 2)
            {
               traceit("irc status=%d, probable connect...\n", rc);
               last_status = 2;
            }
         }
         else
         {
            if (last_status != 1)
            {
               traceit("irc status=%d, probable connect...\n", rc);
               last_status = 1;
            }
         }
         threshold = 0;
      }

      while (((type = ii->getMessageType()) != IDRT_NONE) && keep_running)
      {
         if (type == IDRT_USER)
         {
            ii->receiveUser(user, rptr, gateway, ipaddr);
            if (!user.IsEmpty())
            {
               if (!rptr.IsEmpty() && !gateway.IsEmpty() && !ipaddr.IsEmpty())
               {
                  if (IRC_DEBUG)
                     traceit("C-u:%s,%s,%s,%s\n", user.mb_str(), rptr.mb_str(), gateway.mb_str(), ipaddr.mb_str());

                  pthread_mutex_lock(&irc_data_mutex);

                  user2rptr_map[user] = rptr;
                  rptr2gwy_map[rptr] = gateway;
                  gwy2ip_map[gateway] = ipaddr;

                  pthread_mutex_unlock(&irc_data_mutex);

                  // traceit("%d users, %d repeaters, %d gateways\n", user2rptr_map.size(), rptr2gwy_map.size(), gwy2ip_map.size());

               }
            }
         }
         else
         if (type == IDRT_REPEATER)
         {
            ii->receiveRepeater(rptr, gateway, ipaddr, proto);
            if (!rptr.IsEmpty())
            {
               if (!gateway.IsEmpty() && !ipaddr.IsEmpty())
               {
                  if (IRC_DEBUG)
                     traceit("C-r:%s,%s,%s\n", rptr.mb_str(), gateway.mb_str(), ipaddr.mb_str());

                  pthread_mutex_lock(&irc_data_mutex);

                  rptr2gwy_map[rptr] = gateway;
                  gwy2ip_map[gateway] = ipaddr;

                  pthread_mutex_unlock(&irc_data_mutex);

                  // traceit("%d repeaters, %d gateways\n", rptr2gwy_map.size(), gwy2ip_map.size());
               }
            }
         }
         else
         if (type == IDRT_GATEWAY)
         {
            ii->receiveGateway(gateway, ipaddr, proto);
            if (!gateway.IsEmpty() && !ipaddr.IsEmpty())
            {
               if (IRC_DEBUG)
                  traceit("C-g:%s,%s\n", gateway.mb_str(),ipaddr.mb_str());

               pthread_mutex_lock(&irc_data_mutex);

               gwy2ip_map[gateway] = ipaddr;

               pthread_mutex_unlock(&irc_data_mutex);

               // traceit("%d gateways\n", gwy2ip_map.size());

            }
         }
      }
      req.tv_sec = 0;
      req.tv_nsec = 500000000; // 500 milli
      nanosleep(&req, NULL);
   }
   traceit("get_irc_data thread exiting...\n");
   pthread_exit(NULL);
}

/* return codes: 0=OK(found it), 1=TRY AGAIN, 2=FAILED(bad data) */
static int get_yrcall_rptr_from_cache(char *call, char *arearp_cs, char *zonerp_cs,
                           char *mod, char *ip, char RoU)
{
   user2rptr_type::iterator user_pos = user2rptr_map.end();
   rptr2gwy_type::iterator rptr_pos = rptr2gwy_map.end();
   gwy2ip_type::iterator gwy_pos = gwy2ip_map.end();
   char temp[CALL_SIZE + 1];

   memset(arearp_cs, ' ', CALL_SIZE); arearp_cs[CALL_SIZE] = '\0';
   memset(zonerp_cs, ' ', CALL_SIZE); zonerp_cs[CALL_SIZE] = '\0';
   *mod = ' ';

   /* find the user in the CACHE */
   if (RoU == 'U')
   {
      user_pos = user2rptr_map.find(call);
      if (user_pos != user2rptr_map.end())
      {
         memcpy(arearp_cs, user_pos->second.c_str(), 7);
         *mod = user_pos->second.c_str()[7];
      }
      else
         return 1;
   }
   else
   if (RoU == 'R')
   {
      memcpy(arearp_cs, call, 7);
      *mod = call[7];
   }
   else
   {
      traceit("Invalid specification %c for RoU\n", RoU);
      return 2;
   }

   if ((*mod != 'A') && (*mod != 'B') && (*mod != 'C'))
   {
      traceit("Invalid module %c\n", *mod);
      return 2;
   }

   memcpy(temp, arearp_cs, 7); temp[7] = *mod;
   temp[CALL_SIZE] = '\0';

   rptr_pos = rptr2gwy_map.find(temp);
   if (rptr_pos != rptr2gwy_map.end())
   {
      memcpy(zonerp_cs, rptr_pos->second.c_str(), CALL_SIZE);
      zonerp_cs[CALL_SIZE] = '\0';

      gwy_pos = gwy2ip_map.find(zonerp_cs);
      if (gwy_pos != gwy2ip_map.end())
      {
         strncpy(ip, gwy_pos->second.c_str(),IP_SIZE);
         ip[IP_SIZE] = '\0';
         return 0;
      }
      else
      {
         /* traceit("Could not find IP for Gateway %s\n", zonerp_cs); */
         return 1;
      }
   }
   else
   {
      /* traceit("Could not find Gateway for repeater %s\n", temp); */
      return 1;
   }

   return 2;
}

static bool get_yrcall_rptr(char *call, char *arearp_cs, char *zonerp_cs,
                           char *mod, char *ip, char RoU)
{
   int rc = 2;
   int status = 0;

   pthread_mutex_lock(&irc_data_mutex);
   rc = get_yrcall_rptr_from_cache(call, arearp_cs, zonerp_cs, mod, ip, RoU);
   pthread_mutex_unlock(&irc_data_mutex);
   if (rc == 0)
      return true;
   else
   if (rc == 2)
      return false;

   /* at this point, the data is not in cache */
   /* report the irc status */
   status = ii->getConnectionState();
   // traceit("irc status=%d\n", status);
   if (status != 7)
   {
      traceit("Remote irc database not ready, irc status is not 7, try again\n");
      return false;
   }

   /* request data from irc server */
   if (RoU == 'U')
   {
      traceit("User [%s] not in local cache, try again\n", call);
      /*** YRCALL=KJ4NHFBL ***/
      if (((call[6] == 'A') || (call[6] == 'B') || (call[6] == 'C')) &&
          (call[7] == LINK_CODE))
         traceit("If this was a gateway link request, that is ok\n");

      if (!ii->findUser(call))
      {
         traceit("findUser(%s): Network error\n", call);
         return false;
      }
   }
   else
   if (RoU == 'R')
   {
      traceit("Repeater [%s] not in local cache, try again\n", call);
      if (!ii->findRepeater(call))
      {
         traceit("findRepeater(%s): Network error\n", call);
         return false;
      }
   }
   else
   {
      traceit("Invalid specification %c for RoU\n", RoU);
      return false;
   }

   return false;
}

/* signal catching function */
static void sigCatch(int signum)
{
   /* do NOT do any serious work here */ 
   if ((signum == SIGTERM) || (signum == SIGINT))
      keep_running = false;
   
   return; 
}

/* run the main loop for g2_ircddb */
static void runit()
{
   fd_set fdset;
   struct timeval tv;

   socklen_t fromlen;
   int recvlen;
   int recvlen2;

   short i,j;

   bool result = false;
   int mycall_valid = REG_NOERROR;
   char temp_radio_user[CALL_SIZE + 1];
   char temp_mod;
   time_t t_now;
 
   char arearp_cs[CALL_SIZE + 1];
   char zonerp_cs[CALL_SIZE + 1];
   char ip[IP_SIZE + 1];

   unsigned char save_buf[10];

   int rc = 0;
   char tempfile[FILENAME_MAX + 1];
   pthread_t echo_thread;
   pthread_attr_t attr;
   long num_recs = 0L;
   short int rec_len = 56;

   pthread_t aprs_beacon_thread;
   pthread_t irc_data_thread;

   /* START:  TEXT crap */
   bool new_group[3] = { true, true, true };
   int header_type = 0;
   short to_print[3] = { 0, 0, 0 };
   bool ABC_grp[3] = { false, false, false };
   bool C_seen[3] = { false, false, false };
   unsigned char tmp_txt[3];
   /* END:  TEXT crap */

   int ber_data[3];
   int ber_errs;

   int max_nfds = 0;

   dstar_dv_init();

   if (g2_sock > max_nfds)
      max_nfds = g2_sock;
   if (srv_sock > max_nfds)
      max_nfds = srv_sock;
   traceit("g2=%d, srv=%d, MAX+1=%d\n", 
            g2_sock, srv_sock, max_nfds + 1);

   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   rc = pthread_create(&irc_data_thread,&attr,get_irc_data,(void *)0);
   if (rc != 0)
   {
      traceit("failed to start the get_irc_data thread\n");
      keep_running = false;
   }
   else
      traceit("get_irc_data thread started\n");
   pthread_attr_destroy(&attr);

   /* report to ICOM controller */
   memset(readBuffer, 0, 10);
   memcpy(readBuffer, "INIT", 4);
   readBuffer[6] = 0x73;
   sendto(srv_sock, (char *)readBuffer, 10, 0,(struct sockaddr *)&toRptr,sizeof(struct sockaddr_in));
   traceit("Waiting for ICOM controller...\n");

   while (keep_running)
   {
      fromlen = sizeof(struct sockaddr_in);
      recvlen = recvfrom(srv_sock,(char *)readBuffer,2000,
                         0,(struct sockaddr *)&fromRptr,&fromlen);

      if ((recvlen == 10) &&
          (memcmp(readBuffer, "INIT", 4) == 0) &&
          (readBuffer[6] == 0x72) &&
          (readBuffer[7] == 0x00))
      {

         OLD_REPLY_SEQ = (readBuffer[4] * 256U) + readBuffer[5];
         NEW_REPLY_SEQ = (readBuffer[4] * 256U) + readBuffer[5] + 1;
         G2_COUNTER_OUT =  (readBuffer[4] * 256U) + readBuffer[5] + 1;
         traceit("SYNC: old=%u, new=%u, out=%u\n",
                  OLD_REPLY_SEQ, NEW_REPLY_SEQ, G2_COUNTER_OUT);
         break;
      }
      wxMilliSleep(100);
   }

   traceit("Detected ICOM controller OK\n");
  
   /* start the beacon thread */
   if (SEND_APRS)
   {
      pthread_attr_init(&attr);
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      rc = pthread_create(&aprs_beacon_thread,&attr,send_aprs_beacon,(void *)0);
      if (rc != 0)
         traceit("failed to start the aprs beacon thread\n");
      else
         traceit("APRS beacon thread started\n");
      pthread_attr_destroy(&attr);
   }

   while (keep_running)
   {

      time(&t_now);
      if ((t_now - wd_timer) >= 900)
      {
        ii->kickWatchdog(PACKAGE_REV);
        wd_timer = t_now;
      }

      for (i = 0; i < 3; i++)
      {
         /* echotest recording timed out? */
         if (recd[i].last_time != 0)
         {
            time(&t_now);
            if ((t_now - recd[i].last_time) > ECHOTEST_REC_TIMEOUT)
            {
                traceit("Inactivity on echotest recording mod %d, removing stream id=%d,%d\n",
                         i,recd[i].streamid[0], recd[i].streamid[1]);

                recd[i].streamid[0] = 0x00;
                recd[i].streamid[1] = 0x00;
                recd[i].last_time = 0;
                close(recd[i].fd); recd[i].fd = -1;
                // traceit("Closed echotest audio file:[%s]\n", recd[i].file);

                /* START: echotest thread setup */
                pthread_attr_init(&attr);
                pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                rc = pthread_create(&echo_thread, &attr, echotest, (void *)recd[i].file);
                if (rc != 0)
                {
                   traceit("failed to start echotest thread\n");
                   /* when the echotest thread runs, it deletes the file,
                      Because the echotest thread did NOT start, we delete the file here
                   */
                   unlink(recd[i].file);
                }
                pthread_attr_destroy(&attr);
                /* END: echotest thread setup */
            }
         }

         /* voicemail recording timed out? */
         if (vm[i].last_time != 0)
         {
            time(&t_now);
            if ((t_now - vm[i].last_time) > VOICEMAIL_REC_TIMEOUT)
            {
                traceit("Inactivity on voicemail recording mod %d, removing stream id=%d,%d\n",
                         i,vm[i].streamid[0], vm[i].streamid[1]);

                vm[i].streamid[0] = 0x00;
                vm[i].streamid[1] = 0x00;
                vm[i].last_time = 0;
                close(vm[i].fd); vm[i].fd = -1;
                // traceit("Closed voicemail audio file:[%s]\n", vm[i].file);
            }
         }

         /* any stream going to local repeater timed out? */
         if (toRptr_rp2c[i].last_time != 0)
         {
            time(&t_now);
            if ((t_now - toRptr_rp2c[i].last_time) > FROM_REMOTE_G2_TIMEOUT)
            {
               traceit("Inactivity to local rptr mod index %d, removing stream id %d,%d\n",
                        i, toRptr_rp2c[i].streamid[0], toRptr_rp2c[i].streamid[1]);

               toRptr_rp2c[i].streamid[0] = '\0';
               toRptr_rp2c[i].streamid[1] = '\0';
               toRptr_rp2c[i].adr = 0;
               toRptr_rp2c[i].last_time = 0;
            }
         }

         /* any stream coming from local repeater timed out ? */
         if (band_txt[i].last_time != 0)
         {
            time(&t_now);
            if ((t_now - band_txt[i].last_time) > FROM_LOCAL_RPTR_TIMEOUT)
            {
               /* This local stream never went to a remote system, so trace the timeout */
               if (to_remote_g2[i].toDst4.sin_addr.s_addr == 0)
                  traceit("Inactivity from local rptr band %d, removing stream id %d,%d\n",
                        i, band_txt[i].streamID[0], band_txt[i].streamID[1]);

               band_txt[i].streamID[0] = 0x00;
               band_txt[i].streamID[1] = 0x00;
               band_txt[i].flags[0] = 0x00;
               band_txt[i].flags[1] = 0x00;
               band_txt[i].flags[2] = 0x00;
               band_txt[i].lh_mycall[0] = '\0';
               band_txt[i].lh_sfx[0] = '\0';
               band_txt[i].lh_yrcall[0] = '\0';
               band_txt[i].lh_rpt1[0] = '\0';
               band_txt[i].lh_rpt2[0] = '\0';

               band_txt[i].last_time = 0;

               band_txt[i].txt[0] = '\0';
               band_txt[i].txt_cnt = 0;

               band_txt[i].dest_rptr[0] = '\0';

               band_txt[i].num_dv_frames = 0;
               band_txt[i].num_dv_silent_frames = 0;
               band_txt[i].num_bit_errors = 0;
            }
         }

         /* any stream from local repeater to a remote gateway timed out ? */
         if (to_remote_g2[i].toDst4.sin_addr.s_addr != 0)
         {
            time(&t_now);
            if ((t_now - to_remote_g2[i].last_time) > FROM_LOCAL_RPTR_TIMEOUT)
            {
               traceit("Inactivity from local rptr mod %d, removing stream id %d,%d\n",
                        i, to_remote_g2[i].streamid[0], to_remote_g2[i].streamid[1]);

               memset(&(to_remote_g2[i].toDst4),0,sizeof(struct sockaddr_in));
               to_remote_g2[i].streamid[0] = '\0';
               to_remote_g2[i].streamid[1] = '\0';
               to_remote_g2[i].last_time = 0;
            }
         }
      }

      /* wait 20 ms max */
      FD_ZERO(&fdset);
      FD_SET(g2_sock,&fdset);
      FD_SET(srv_sock,&fdset);
      tv.tv_sec = 0;
      tv.tv_usec = 20000; /* 20 ms */
      (void)select(max_nfds + 1,&fdset,0,0,&tv);

      /* process packets coming from remote G2 */
      if (FD_ISSET(g2_sock, &fdset))
      {
         fromlen = sizeof(struct sockaddr_in);
         recvlen2 = recvfrom(g2_sock,(char *)readBuffer2,2000,
                            0,(struct sockaddr *)&fromDst4,&fromlen);

         if ( ((recvlen2 == 56) || 
               (recvlen2 == 27)) &&
              (memcmp(readBuffer2, "DSVT", 4) == 0) &&
              ((readBuffer2[4] == 0x10) ||   /* header */
               (readBuffer2[4] == 0x20)) &&  /* details */
              (readBuffer2[8] == 0x20))      /* voice type */
         {
            if (recvlen2 == 56) 
            {
                  i = -1;
                  if (readBuffer2[25] == 'A')
                     i = 0;
                  else
                  if (readBuffer2[25] == 'B')
                     i = 1;
                  else
                  if (readBuffer2[25] == 'C')
                     i = 2;

                  if (i >= 0)
                  {
                     if ((toRptr_rp2c[i].last_time == 0) && (band_txt[i].last_time == 0) &&
                         ((readBuffer2[15] == 0x00) ||
                          (readBuffer2[15] == 0x01) || /* allow the announcements from g2_link */
                          (readBuffer2[15] == 0x08) ||
                          (readBuffer2[15] == 0x20) ||
                          (readBuffer2[15] == 0x28) ||
                          (readBuffer2[15] == 0x40)))
                     {
                        if (QSO_DETAILS)
                           traceit("START from g2: streamID=%d,%d, flags=%02x:%02x:%02x, my=%.8s, sfx=%.4s, ur=%.8s, rpt1=%.8s, rpt2=%.8s, %d bytes fromIP=%s\n",
                                   readBuffer2[12],readBuffer2[13],
                                   readBuffer2[15], readBuffer2[16], readBuffer2[17],
                                   &readBuffer2[42],
                                   &readBuffer2[50], &readBuffer2[34],
                                   &readBuffer2[18], &readBuffer2[26],
                                   recvlen2,inet_ntoa(fromDst4.sin_addr));

                        memcpy(readBuffer,"DSTR", 4);
                        readBuffer[5] = (unsigned char)(G2_COUNTER_OUT & 0xff);
                        readBuffer[4] = (unsigned char)((G2_COUNTER_OUT >> 8) & 0xff);
                        readBuffer[6] = 0x73;
                        readBuffer[7] = 0x12;
                        readBuffer[8] = 0x00;
                        readBuffer[9] = 0x30;
                        readBuffer[10] = 0x20;
                        memcpy(readBuffer + 11, readBuffer2 + 9, 47);
                        sendto(srv_sock, (char *)readBuffer,58,0,
                               (struct sockaddr *)&toRptr,
                               sizeof(struct sockaddr_in));

                        /* save the header */
                        memcpy(toRptr_rp2c[i].saved_hdr, readBuffer, 58);
                        toRptr_rp2c[i].saved_adr = fromDst4.sin_addr.s_addr;

                        /* This is the active streamid */
                        toRptr_rp2c[i].streamid[0] = readBuffer2[12];
                        toRptr_rp2c[i].streamid[1] = readBuffer2[13];
                        toRptr_rp2c[i].adr = fromDst4.sin_addr.s_addr;

                        /* time it, in case stream times out */
                        time(&toRptr_rp2c[i].last_time);

                        /* bump the G2 counter */
                        G2_COUNTER_OUT ++;

                     }
                  }
            }
            else
            {
               if ((readBuffer2[14] & 0x40) != 0)
               {
                  if (QSO_DETAILS)
                     traceit("END from g2: streamID=%d,%d, %d bytes from IP=%s\n",
                             readBuffer2[12],readBuffer2[13],
                             recvlen2,inet_ntoa(fromDst4.sin_addr));
               }

               for (i = 0; i < 3; i++)
               {
                  /* streamid match ? */
                  if ((memcmp(toRptr_rp2c[i].streamid, readBuffer2 + 12, 2) == 0) &&
                      (toRptr_rp2c[i].adr == fromDst4.sin_addr.s_addr))
                  {
                     memcpy(readBuffer,"DSTR", 4);
                     readBuffer[5] = (unsigned char)(G2_COUNTER_OUT & 0xff);
                     readBuffer[4] = (unsigned char)((G2_COUNTER_OUT >> 8) & 0xff);
                     readBuffer[6] = 0x73;
                     readBuffer[7] = 0x12;
                     readBuffer[8] = 0x00;
                     readBuffer[9] = 0x13;
                     readBuffer[10] = 0x20;
                     memcpy(readBuffer + 11, readBuffer2 + 9, 18);

                     sendto(srv_sock, (char *)readBuffer,29,0,
                            (struct sockaddr *)&toRptr,
                            sizeof(struct sockaddr_in));

                     /* timeit */
                     time(&toRptr_rp2c[i].last_time);

                     /* bump G2 counter */
                     G2_COUNTER_OUT ++;

                     /* End of stream ? */
                     if ((readBuffer2[14] & 0x40) != 0)
                     {
                        /* clear the saved header */
                        memset(toRptr_rp2c[i].saved_hdr, 0, sizeof(toRptr_rp2c[i].saved_hdr));
                        toRptr_rp2c[i].saved_adr = 0;

                        toRptr_rp2c[i].last_time = 0;
                        toRptr_rp2c[i].streamid[0] = '\0';
                        toRptr_rp2c[i].streamid[1] = '\0';
                        toRptr_rp2c[i].adr = 0;
                     }
                     break;
                  }
               }

               /* no match ? */
               if ((i == 3) && REGEN_HDR)
               {
                  /* check if this a continuation of audio that timed out */

                  if ((readBuffer2[14] & 0x40) != 0)
                     ;  /* we do not care about end-of-QSO */
                  else
                  {
                     /* for which repeater this stream has timed out ?  */
                     for (i = 0; i < 3; i++)
                     {
                        /* match saved stream ? */
                        if ((memcmp(toRptr_rp2c[i].saved_hdr + 14, readBuffer2 + 12, 2) == 0) &&
                            (toRptr_rp2c[i].saved_adr == fromDst4.sin_addr.s_addr))
                        {
                           /* repeater module is inactive ?  */
                           if ((toRptr_rp2c[i].last_time == 0) && (band_txt[i].last_time == 0))
                           {
                              traceit("Re-generating header for streamID=%d,%d\n", readBuffer2[12],readBuffer2[13]);

                              toRptr_rp2c[i].saved_hdr[5] = (unsigned char)(G2_COUNTER_OUT & 0xff);
                              toRptr_rp2c[i].saved_hdr[4] = (unsigned char)((G2_COUNTER_OUT >> 8) & 0xff);

                              /* re-generate/send the header */
                              sendto(srv_sock, (char *)toRptr_rp2c[i].saved_hdr,58,0,
                                     (struct sockaddr *)&toRptr,
                                     sizeof(struct sockaddr_in));

                              /* bump the G2 counter */
                              G2_COUNTER_OUT ++;

                              /* send this audio packet to repeater */
                              memcpy(readBuffer,"DSTR", 4);
                              readBuffer[5] = (unsigned char)(G2_COUNTER_OUT & 0xff);
                              readBuffer[4] = (unsigned char)((G2_COUNTER_OUT >> 8) & 0xff);
                              readBuffer[6] = 0x73;
                              readBuffer[7] = 0x12;
                              readBuffer[8] = 0x00;
                              readBuffer[9] = 0x13;
                              readBuffer[10] = 0x20;
                              memcpy(readBuffer + 11, readBuffer2 + 9, 18);

                              sendto(srv_sock, (char *)readBuffer,29,0,
                                     (struct sockaddr *)&toRptr,
                                     sizeof(struct sockaddr_in));

                              /* make sure that any more audio arriving will be accepted */
                              toRptr_rp2c[i].streamid[0] = readBuffer2[12];
                              toRptr_rp2c[i].streamid[1] = readBuffer2[13];
                              toRptr_rp2c[i].adr = fromDst4.sin_addr.s_addr;

                              /* time it, in case stream times out */
                              time(&toRptr_rp2c[i].last_time);

                              /* bump the G2 counter */
                              G2_COUNTER_OUT ++;
                           }
                           break;
                        }
                     }
                  }
               }
            }
         }
         FD_CLR (g2_sock,&fdset);
      }

      /* process data coming from local repeater modules */
      if (FD_ISSET(srv_sock, &fdset))
      {
         fromlen = sizeof(struct sockaddr_in);
         recvlen = recvfrom(srv_sock,(char *)readBuffer,2000,
                            0,(struct sockaddr *)&fromRptr,&fromlen);

         /* Lets process those annoying packets from controller */

         /* ACK from rptr */
         if ((recvlen == 10) &&
             (readBuffer[6] == 0x72) &&
             (memcmp(readBuffer,"DSTR", 4) == 0))
         {
            NEW_REPLY_SEQ = (readBuffer[4] * 256U) + readBuffer[5];

            if (NEW_REPLY_SEQ == OLD_REPLY_SEQ)
            {
               /*
               traceit("Re-SYNC before: old=%u, new=%u, out=%u\n",
                  OLD_REPLY_SEQ, NEW_REPLY_SEQ, G2_COUNTER_OUT);
               */

               G2_COUNTER_OUT =  NEW_REPLY_SEQ;
               OLD_REPLY_SEQ = NEW_REPLY_SEQ - 1;

               /*
               traceit("Re-SYNC after: old=%u, new=%u, out=%u\n",
                  OLD_REPLY_SEQ, NEW_REPLY_SEQ, G2_COUNTER_OUT);
               */
            }
            else
               OLD_REPLY_SEQ = NEW_REPLY_SEQ;
         }
         else
         /* server packet S ? */
         if ((readBuffer[6] == 0x73) &&
             (readBuffer[7] == 0x21) &&
             (memcmp(readBuffer,"DSTR", 4) == 0))
         {
            TEMP_SEQ = (readBuffer[4] * 256U) + readBuffer[5];
            readBuffer[4] = TEMP_SEQ / 256U;
            readBuffer[5] = TEMP_SEQ % 256U;
            readBuffer[6] = 0x72;
            memset(readBuffer + 7, 0, 3);
            sendto(srv_sock, (char *)readBuffer, 10, 0,(struct sockaddr *)&toRptr,sizeof(struct sockaddr_in));
         }
         else
         /* keepalive from repeater controller ? */
         if ((readBuffer[6] == 0x73) &&
             (readBuffer[7] == 0x00) &&
             (memcmp(readBuffer,"DSTR", 4) == 0))
         {
            TEMP_SEQ = (readBuffer[4] * 256U) + readBuffer[5];
            readBuffer[4] = TEMP_SEQ / 256U;
            readBuffer[5] = TEMP_SEQ % 256U;
            readBuffer[6] = 0x72;
            memset(readBuffer + 7, 0, 3);
            sendto(srv_sock, (char *)readBuffer, 10,0, (struct sockaddr *)&toRptr,sizeof(struct sockaddr_in));
         }
         else
         /* DD, variable length ? */
         if ((readBuffer[6] == 0x73) &&
             (readBuffer[7] == 0x11) &&
             (memcmp(readBuffer,"DSTR", 4) == 0))
         {
            TEMP_SEQ = (readBuffer[4] * 256U) + readBuffer[5];
            readBuffer[4] = TEMP_SEQ / 256U;
            readBuffer[5] = TEMP_SEQ % 256U;
            readBuffer[6] = 0x72;
            memset(readBuffer + 7, 0, 3);
            sendto(srv_sock, (char *)readBuffer, 10,0, (struct sockaddr *)&toRptr,sizeof(struct sockaddr_in));
         }
         else
         /* DV */
         if ( ((recvlen == 58) || 
               (recvlen == 29) || 
               (recvlen == 32)) &&
              (readBuffer[6] == 0x73) &&
              (readBuffer[7] == 0x12) &&
              (memcmp(readBuffer,"DSTR", 4) == 0) &&
              (readBuffer[10] == 0x20) &&
              (readBuffer[8] == 0x00) &&     
              ((readBuffer[9] == 0x30) ||    /* 48 bytes follow */
               (readBuffer[9] == 0x13) ||    /* 19 bytes follow */
               (readBuffer[9] == 0x16)) )    /* 22 bytes follow */
         {
            memcpy(save_buf, "DSTR", 4);
            TEMP_SEQ = (readBuffer[4] * 256U) + readBuffer[5];
            save_buf[4] = TEMP_SEQ / 256U;
            save_buf[5] = TEMP_SEQ % 256U;
            save_buf[6] = 0x72;
            save_buf[7] = 0x00;
            memset(save_buf + 8, 0, 2);
            sendto(srv_sock, (char *)save_buf, 10,0, (struct sockaddr *)&toRptr,sizeof(struct sockaddr_in));

            if (recvlen == 58)
            {
                  if (QSO_DETAILS)
                     traceit("START from rptr: cntr=%02x %02x, streamID=%d,%d, flags=%02x:%02x:%02x, my=%.8s, sfx=%.4s, ur=%.8s, rpt1=%.8s, rpt2=%.8s, %d bytes fromIP=%s\n",
                              readBuffer[4], readBuffer[5],
                              readBuffer[14], readBuffer[15],
                              readBuffer[17], readBuffer[18], readBuffer[19],
                              readBuffer + 44, readBuffer + 52, readBuffer + 36,
                              readBuffer + 28, readBuffer + 20, recvlen, inet_ntoa(fromRptr.sin_addr));

                  if ((memcmp(readBuffer + 28, OWNER, 7) == 0) &&  /* rpt1 is this repeater */
                      /*** (memcmp(readBuffer + 44, OWNER, 7) != 0) && ***/  /* MYCALL is NOT this repeater */
                      ((readBuffer[17] == 0x00) ||                 /* normal */
                       (readBuffer[17] == 0x08) ||                 /* EMR */
                       (readBuffer[17] == 0x20) ||                 /* BREAK */
                       (readBuffer[17] == 0x28)))                  /* EMR + BREAK */
                  {
                     i = -1;
                     if (readBuffer[35] == 'A')
                        i = 0;
                     else
                     if (readBuffer[35] == 'B')
                        i = 1;
                     else
                     if (readBuffer[35] == 'C')
                        i = 2;

                     if (i >= 0)
                     {
                        dtmf_last_frame[i] = 0;
                        dtmf_counter[i] = 0;
                        memset(dtmf_buf[i], 0, sizeof(dtmf_buf[i]));
                        dtmf_buf_count[i] = 0;

                        /* Initialize the LAST HEARD data for the band */

                        band_txt[i].streamID[0] = readBuffer[14];
                        band_txt[i].streamID[1] = readBuffer[15];

                        band_txt[i].flags[0] = readBuffer[17];
                        band_txt[i].flags[1] = readBuffer[18];
                        band_txt[i].flags[2] = readBuffer[19];

                        memcpy(band_txt[i].lh_mycall, readBuffer + 44, CALL_SIZE);
                        band_txt[i].lh_mycall[CALL_SIZE] = '\0';

                        memcpy(band_txt[i].lh_sfx, readBuffer + 52, 4);
                        band_txt[i].lh_sfx[4] = '\0';

                        memcpy(band_txt[i].lh_yrcall, readBuffer + 36, CALL_SIZE);
                        band_txt[i].lh_yrcall[CALL_SIZE] = '\0';

                        memcpy(band_txt[i].lh_rpt1, readBuffer + 28, CALL_SIZE);
                        band_txt[i].lh_rpt1[CALL_SIZE] = '\0';

                        memcpy(band_txt[i].lh_rpt2, readBuffer + 20, CALL_SIZE);
                        band_txt[i].lh_rpt2[CALL_SIZE] = '\0';

                        time(&band_txt[i].last_time);

                        band_txt[i].txt[0] = '\0';
                        band_txt[i].txt_cnt = 0;
                        band_txt[i].txt_stats_sent = false;

                        band_txt[i].dest_rptr[0] = '\0';

                        /* try to process GPS mode: GPRMC and ID */
                        band_txt[i].temp_line[0] = '\0';
                        band_txt[i].temp_line_cnt = 0;
                        band_txt[i].gprmc[0] = '\0';
                        band_txt[i].gpid[0] = '\0';
                        band_txt[i].is_gps_sent = false;
                        // band_txt[i].gps_last_time = 0; DO NOT reset it

                        new_group[i] = true;
                        to_print[i] = 0;
                        ABC_grp[i] = false;

                        band_txt[i].num_dv_frames = 0;
                        band_txt[i].num_dv_silent_frames = 0;
                        band_txt[i].num_bit_errors = 0;

                        /* select the band for aprs processing, and lock on the stream ID */
                        if (SEND_APRS)
                           aprs_select_band(i, readBuffer + 14);
                     }
                  }

                  /* Is MYCALL valid ? */
                  memset(temp_radio_user, ' ', CALL_SIZE);
                  memcpy(temp_radio_user, readBuffer + 44, 8);
                  temp_radio_user[CALL_SIZE] = '\0';
                  
                  mycall_valid = regexec(&preg, temp_radio_user, 0, NULL, 0);
                  if (mycall_valid == REG_NOERROR)
                     ; // traceit("MYCALL [%s] passed IRC expression validation\n", temp_radio_user);
                  else
                  {
                     if (mycall_valid == REG_NOMATCH)
                        traceit("MYCALL [%s] failed IRC expression validation\n", temp_radio_user);
                     else
                        traceit("Failed to validate MYCALL [%s], regexec error=%d\n", temp_radio_user, mycall_valid);
                  }

                  if (mycall_valid == REG_NOERROR)
                     sendto(g2_sock, (char *)readBuffer, recvlen,0,
                            (struct sockaddr *)&plug,
                            sizeof(struct sockaddr_in));

                  if ((mycall_valid == REG_NOERROR) &&
                      (memcmp(readBuffer + 36, "XRF", 3) != 0) &&             /* not a reflector */
                      (memcmp(readBuffer + 36, "REF", 3) != 0) &&             /* not a reflector */
                      (memcmp(readBuffer + 36, "DCS", 3) != 0) &&             /* not a reflector */
                      (readBuffer[36] != ' ') &&                              /* must have something */
                      (memcmp(readBuffer + 36, "CQCQCQ", 6) != 0))            /* urcall is NOT CQCQCQ */
                  {
                     if ((readBuffer[36] == '/') &&                           /* urcall starts with a slash */
                         (memcmp(readBuffer + 28, OWNER, 7) == 0) &&          /* rpt1 is this repeater */
                         ((readBuffer[35] == 'A') || 
                          (readBuffer[35] == 'B') || 
                          (readBuffer[35] == 'C')) &&                         /* mod is A,B,C */
                         (memcmp(readBuffer + 20, OWNER, 7) == 0) &&          /* rpt2 is this repeater */
                         (readBuffer[27] == 'G') &&                           /* local Gateway */
                         /*** (memcmp(readBuffer + 44, OWNER, 7) != 0) && ***/  /* mycall is NOT this repeater */

                         ((readBuffer[17] == 0x00) ||                         /* normal */
                          (readBuffer[17] == 0x08) ||                         /* EMR */
                          (readBuffer[17] == 0x20) ||                         /* BK */
                          (readBuffer[17] == 0x28))                           /* EMR + BK */
                        )
                     {
                        if (memcmp(readBuffer + 37, OWNER, 6) != 0)           /* the value after the slash in urcall, is NOT this repeater */
                        {
                           i = -1;
                           if (readBuffer[35] == 'A')
                              i = 0; 
                           else
                           if (readBuffer[35] == 'B')
                              i = 1;
                           else
                           if (readBuffer[35] == 'C')
                              i = 2;

                           if (i >= 0)
                           {
                              /* one radio user on a repeater module at a time */
                              if (to_remote_g2[i].toDst4.sin_addr.s_addr == 0)
                              { 
                                 /* YRCALL=/repeater + mod */
                                 /* YRCALL=/KJ4NHFB */
 
                                 memset(temp_radio_user, ' ', CALL_SIZE);
                                 memcpy(temp_radio_user, readBuffer + 37, 6);
                                 temp_radio_user[6] = ' ';
                                 temp_radio_user[7] = readBuffer[43];
                                 if (temp_radio_user[7] == ' ')
                                    temp_radio_user[7] = 'A';
                                 temp_radio_user[CALL_SIZE] = '\0';

                                 result = get_yrcall_rptr(temp_radio_user, arearp_cs, zonerp_cs, &temp_mod, ip, 'R');
                                 if (result) /* it is a repeater */      
                                 {
                                    /* set the destination */
                                    memcpy(to_remote_g2[i].streamid, readBuffer + 14, 2);
                                    memset(&(to_remote_g2[i].toDst4),0,sizeof(struct sockaddr_in));
                                    to_remote_g2[i].toDst4.sin_family = AF_INET;
                                    to_remote_g2[i].toDst4.sin_port = htons(G2_EXTERNAL_PORT);
                                    to_remote_g2[i].toDst4.sin_addr.s_addr = inet_addr(ip);

                                    memcpy(readBuffer2, "DSVT", 4);
                                    readBuffer2[4] = 0x10;
                                    readBuffer2[5] = 0x00;
                                    readBuffer2[6] = 0x00;
                                    readBuffer2[7] = 0x00;
                                    readBuffer2[8] =  readBuffer[10];
                                    readBuffer2[9] =  readBuffer[11];
                                    readBuffer2[10] = readBuffer[12];
                                    readBuffer2[11] = readBuffer[13];
                                    memcpy(readBuffer2 + 12, readBuffer + 14, 44);
                                    /* set rpt1 */
                                    memset(readBuffer2 + 18, ' ', CALL_SIZE);
                                    memcpy(readBuffer2 + 18, arearp_cs, strlen(arearp_cs));
                                    readBuffer2[25] = temp_mod;
                                    /* set rpt2 */
                                    memset(readBuffer2 + 26, ' ', CALL_SIZE);
                                    memcpy(readBuffer2 + 26, zonerp_cs, strlen(zonerp_cs));
                                    readBuffer2[33] = 'G';
                                    /* set yrcall, can NOT let it be slash and repeater + module */
                                    memcpy(readBuffer2 + 34, "CQCQCQ  ", 8);

                                    /* set PFCS */
                                    calcPFCS(readBuffer2, 56);

                                    /*
                                       The remote repeater has been set, lets fill in the dest_rptr
                                       so that later we can send that to the LIVE web site
                                    */
                                    memcpy(band_txt[i].dest_rptr, readBuffer2 + 18, CALL_SIZE);
                                    band_txt[i].dest_rptr[CALL_SIZE] = '\0';

                                    /* send to remote gateway */
                                    for (j = 0; j < 5; j++)
                                       sendto(g2_sock, (char *)readBuffer2, 56,
                                              0,(struct sockaddr *)&(to_remote_g2[i].toDst4),
                                              sizeof(struct sockaddr_in));

                                    traceit("Routing to IP=%s, streamID=%d,%d, my=%.8s, sfx=%.4s, ur=%.8s, rpt1=%.8s, rpt2=%.8s, %d bytes\n",
                                               inet_ntoa(to_remote_g2[i].toDst4.sin_addr),
                                               readBuffer2[12],readBuffer2[13],&readBuffer2[42],
                                               &readBuffer2[50], &readBuffer2[34],
                                               &readBuffer2[18], &readBuffer2[26],
                                               56);

                                    time(&(to_remote_g2[i].last_time));
                                 }
                              }
                           }
                        }
                     }
                     else
                     if ((memcmp(readBuffer + 36, OWNER, 7) != 0) &&             /* urcall is not this repeater */
                         (memcmp(readBuffer + 28, OWNER, 7) == 0) &&             /* rpt1 is this repeater */
                         ((readBuffer[35] == 'A') || 
                          (readBuffer[35] == 'B') || 
                          (readBuffer[35] == 'C')) &&                            /* mod is A,B,C */
                         (memcmp(readBuffer + 20, OWNER, 7) == 0) &&             /* rpt2 is this repeater */
                         (readBuffer[27] == 'G') &&                              /* local Gateway */
                         /*** (memcmp(readBuffer + 44, OWNER, 7) != 0) && ***/   /* mycall is NOT this repeater */

                         ((readBuffer[17] == 0x00) ||                         /* normal */
                          (readBuffer[17] == 0x08) ||                         /* EMR */
                          (readBuffer[17] == 0x20) ||                         /* BK */
                          (readBuffer[17] == 0x28))                           /* EMR + BK */
                        )                            
                     {

                        memset(temp_radio_user, ' ', CALL_SIZE);
                        memcpy(temp_radio_user, readBuffer + 36, CALL_SIZE);
                        temp_radio_user[CALL_SIZE] = '\0';
                        result = get_yrcall_rptr(temp_radio_user, arearp_cs, zonerp_cs, &temp_mod, ip, 'U');
                        if (result)
                        {
                           /* destination is a remote system */
                           if (memcmp(zonerp_cs, OWNER, 7) != 0)
                           {
                              i = -1;
                              if (readBuffer[35] == 'A')
                                 i = 0;
                              else
                              if (readBuffer[35] == 'B')
                                 i = 1;
                              else
                              if (readBuffer[35] == 'C')
                                 i = 2;

                              if (i >= 0)
                              {
                                 /* one radio user on a repeater module at a time */
                                 if (to_remote_g2[i].toDst4.sin_addr.s_addr == 0)
                                 {
                                    /* set the destination */
                                    memcpy(to_remote_g2[i].streamid, readBuffer + 14, 2);
                                    memset(&(to_remote_g2[i].toDst4),0,sizeof(struct sockaddr_in));
                                    to_remote_g2[i].toDst4.sin_family = AF_INET;
                                    to_remote_g2[i].toDst4.sin_port = htons(G2_EXTERNAL_PORT);
                                    to_remote_g2[i].toDst4.sin_addr.s_addr = inet_addr(ip);

                                    memcpy(readBuffer2, "DSVT", 4);
                                    readBuffer2[4] = 0x10;
                                    readBuffer2[5] = 0x00;
                                    readBuffer2[6] = 0x00;
                                    readBuffer2[7] = 0x00;
                                    readBuffer2[8] =  readBuffer[10];
                                    readBuffer2[9] =  readBuffer[11];
                                    readBuffer2[10] = readBuffer[12];
                                    readBuffer2[11] = readBuffer[13];
                                    memcpy(readBuffer2 + 12, readBuffer + 14, 44);
                                    /* set rpt1 */
                                    memset(readBuffer2 + 18, ' ', CALL_SIZE);
                                    memcpy(readBuffer2 + 18, arearp_cs, strlen(arearp_cs));
                                    readBuffer2[25] = temp_mod;
                                    /* set rpt2 */
                                    memset(readBuffer2 + 26, ' ', CALL_SIZE);
                                    memcpy(readBuffer2 + 26, zonerp_cs, strlen(zonerp_cs));
                                    readBuffer2[33] = 'G';
                                    /* set PFCS */
                                    calcPFCS(readBuffer2, 56);

                                    /*
                                       The remote repeater has been set, lets fill in the dest_rptr
                                       so that later we can send that to the LIVE web site
                                    */
                                    memcpy(band_txt[i].dest_rptr, readBuffer2 + 18, CALL_SIZE);
                                    band_txt[i].dest_rptr[CALL_SIZE] = '\0';

                                    /* send to remote gateway */
                                    for (j = 0; j < 5; j++)
                                       sendto(g2_sock, (char *)readBuffer2, 56,
                                           0,(struct sockaddr *)&(to_remote_g2[i].toDst4),
                                           sizeof(struct sockaddr_in));

                                    traceit("Routing to IP=%s, streamID=%d,%d, my=%.8s, sfx=%.4s, ur=%.8s, rpt1=%.8s, rpt2=%.8s, %d bytes\n",
                                          inet_ntoa(to_remote_g2[i].toDst4.sin_addr),
                                          readBuffer2[12],readBuffer2[13],&readBuffer2[42],
                                          &readBuffer2[50], &readBuffer2[34],
                                          &readBuffer2[18], &readBuffer2[26],
                                          56);

                                    time(&(to_remote_g2[i].last_time));
                                 }
                              }
                           }
                           else
                           {
                              i = -1;
                              if (readBuffer[35] == 'A')
                                 i = 0;
                              else
                              if (readBuffer[35] == 'B')
                                 i = 1;
                              else
                              if (readBuffer[35] == 'C')
                                 i = 2;

                              if (i >= 0)
                              {
                                 /* the user we are trying to contact is on our gateway */
                                 /* make sure they are on a different module */
                                 if (temp_mod != readBuffer[35])
                                 {
                                    /*
                                       The remote repeater has been set, lets fill in the dest_rptr
                                       so that later we can send that to the LIVE web site
                                    */
                                    memcpy(band_txt[i].dest_rptr, readBuffer + 20, CALL_SIZE);
                                    band_txt[i].dest_rptr[7] = temp_mod;
                                    band_txt[i].dest_rptr[CALL_SIZE] = '\0';

                                    i = -1;
                                    if (temp_mod == 'A')
                                       i = 0;
                                    else
                                    if (temp_mod == 'B')
                                       i = 1;
                                    else
                                    if (temp_mod == 'C')
                                       i = 2;

                                    /* valid destination repeater module? */
                                    if (i >= 0)
                                    {
                                       if ((toRptr_rp2c[i].last_time == 0) && (band_txt[i].last_time == 0))
                                       {
                                          traceit("CALLmode cross-banding from mod %c to %c\n",  readBuffer[35], temp_mod);

                                          readBuffer[27] = temp_mod;
                                          readBuffer[35] = 'G';
                                          calcPFCS(readBuffer, 58);

                                          sendto(srv_sock, (char *)readBuffer,58,0,
                                                 (struct sockaddr *)&toRptr,
                                                 sizeof(struct sockaddr_in));

                                          /* This is the active streamid */
                                          toRptr_rp2c[i].streamid[0] = readBuffer[14];
                                          toRptr_rp2c[i].streamid[1] = readBuffer[15];
                                          toRptr_rp2c[i].adr = fromRptr.sin_addr.s_addr;

                                          /* time it, in case stream times out */
                                          time(&toRptr_rp2c[i].last_time);

                                          /* bump the G2 counter */
                                          G2_COUNTER_OUT ++;
                                       }
                                    }
                                 }
                                 else
                                    traceit("icom rule: no routing from %.8s to %s%c\n", readBuffer + 28, arearp_cs,temp_mod);
                              }
                           }
                        }
                     }
                  }
                  else
                  if ((readBuffer[43] == '0') && 
                      (readBuffer[42] == CLEAR_VM_CODE) &&
                      (readBuffer[36] == ' '))
                  {
                     i = -1;
                     if (readBuffer[35] == 'A')
                        i = 0;
                     else
                     if (readBuffer[35] == 'B')
                        i = 1;
                     else
                     if (readBuffer[35] == 'C')
                        i = 2;

                     if (i >= 0)
                     {
                        /* voicemail file is closed */
                        if ((vm[i].fd == -1) && (vm[i].file[0] != '\0'))
                        {
                           unlink(vm[i].file);
                           traceit("removed voicemail file: %s\n", vm[i].file);
                           vm[i].file[0] = '\0';
                        }
                        else
                           traceit("No voicemail to clear or still recording\n");
                     }
                  }
                  else
                  if ((readBuffer[43] == '0') &&
                      (readBuffer[42] == RECALL_VM_CODE) &&
                      (readBuffer[36] == ' '))
                  {
                     i = -1;
                     if (readBuffer[35] == 'A')
                        i = 0;
                     else
                     if (readBuffer[35] == 'B')
                        i = 1;
                     else
                     if (readBuffer[35] == 'C')
                        i = 2;

                     if (i >= 0)
                     {
                        /* voicemail file is closed */
                        if ((vm[i].fd == -1) && (vm[i].file[0] != '\0'))
                        {
                           pthread_attr_init(&attr);
                           pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                           rc = pthread_create(&echo_thread, &attr, echotest, (void *)vm[i].file);
                           if (rc != 0)
                              traceit("failed to start playing back voicemail\n");
                           pthread_attr_destroy(&attr);
                        }
                        else
                          traceit("No voicemail to recall or still recording\n");
                     }
                  }
                  else
                  if ((readBuffer[43] == '0') &&
                      (readBuffer[42] == STORE_VM_CODE) &&
                      (readBuffer[36] == ' '))
                  {
                     i = -1;
                     if (readBuffer[35] == 'A')
                        i = 0;
                     else
                     if (readBuffer[35] == 'B')
                        i = 1;
                     else
                     if (readBuffer[35] == 'C')
                        i = 2;

                     if (i >= 0)
                     {
                        if (vm[i].fd >= 0)
                           traceit("Already recording for voicemail on mod %d\n", i);
                        else
                        {
                           memset(tempfile, '\0', sizeof(tempfile));
                           snprintf(tempfile, FILENAME_MAX, "%s/%c_%s",
                                 ECHOTEST_DIR,
                                 readBuffer[35],
                                 "voicemail.dat");

                           vm[i].fd = open(tempfile,
                                        O_CREAT | O_WRONLY | O_TRUNC | O_APPEND,
                                        S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
                           if (vm[i].fd < 0)
                              traceit("Failed to create file %s for voicemail\n", tempfile);
                           else
                           {
                              strcpy(vm[i].file, tempfile);
                              traceit("Recording mod %c for voicemail into file:[%s]\n",
                                      readBuffer[35],
                                      vm[i].file);

                              time(&vm[i].last_time);
                              memcpy(vm[i].streamid, readBuffer + 14, 2);

                              memcpy(recbuf, "DSVT", 4);
                              recbuf[4] = 0x10;
                              recbuf[5] = 0x00;
                              recbuf[6] = 0x00;
                              recbuf[7] = 0x00;
                              recbuf[8] =  readBuffer[10];
                              recbuf[9] =  readBuffer[11];
                              recbuf[10] = readBuffer[12];
                              recbuf[11] = readBuffer[13];
                              memcpy(recbuf + 12, readBuffer + 14, 44);
                              memset(recbuf + 18, ' ', CALL_SIZE);
                              memcpy(recbuf + 18, OWNER, strlen(OWNER));
                              recbuf[25] = readBuffer[35];
                              memset(recbuf + 26, ' ', CALL_SIZE);
                              memcpy(recbuf + 26,  OWNER, strlen(OWNER));
                              recbuf[33] = 'G';
                              memcpy(recbuf + 34, "CQCQCQ  ", 8);

                              calcPFCS(recbuf,56);

                              rec_len = 56;
                              write(vm[i].fd, "DVTOOL", 6);
                              write(vm[i].fd, &num_recs, 4);
                              write(vm[i].fd, &rec_len, 2);
                              write(vm[i].fd, (char *)recbuf, rec_len);
                           }
                        }
                     }
                  }
                  else
                  if ((readBuffer[43] == ECHO_CODE) &&
                      (readBuffer[36] == ' '))
                  {
                     i = -1;
                     if (readBuffer[35] == 'A')
                        i = 0;
                     else
                     if (readBuffer[35] == 'B')
                        i = 1;
                     else
                     if (readBuffer[35] == 'C')
                        i = 2;

                     if (i >= 0)
                     {
                        if (recd[i].fd >= 0)
                           traceit("Already recording for echotest on mod %d\n", i);
                        else
                        {
                           memset(tempfile, '\0', sizeof(tempfile));
                           snprintf(tempfile, FILENAME_MAX, "%s/%c_%s",
                                 ECHOTEST_DIR,
                                 readBuffer[35],
                                 "echotest.dat");

                           recd[i].fd = open(tempfile,
                                           O_CREAT | O_WRONLY | O_EXCL | O_TRUNC | O_APPEND,
                                           S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
                           if (recd[i].fd < 0)
                              traceit("Failed to create file %s for echotest\n", tempfile);
                           else
                           {
                              strcpy(recd[i].file, tempfile);
                              traceit("Recording mod %c for echotest into file:[%s]\n",
                                    readBuffer[35],
                                    recd[i].file);

                              time(&recd[i].last_time);
                              memcpy(recd[i].streamid, readBuffer + 14, 2);

                              memcpy(recbuf, "DSVT", 4);
                              recbuf[4] = 0x10;
                              recbuf[5] = 0x00;
                              recbuf[6] = 0x00;
                              recbuf[7] = 0x00;
                              recbuf[8] =  readBuffer[10];
                              recbuf[9] =  readBuffer[11];
                              recbuf[10] = readBuffer[12];
                              recbuf[11] = readBuffer[13];
                              memcpy(recbuf + 12, readBuffer + 14, 44);
                              memset(recbuf + 18, ' ', CALL_SIZE);
                              memcpy(recbuf + 18, OWNER, strlen(OWNER));
                              recbuf[25] = readBuffer[35];
                              memset(recbuf + 26, ' ', CALL_SIZE);
                              memcpy(recbuf + 26,  OWNER, strlen(OWNER));
                              recbuf[33] = 'G';
                              memcpy(recbuf + 34, "CQCQCQ  ", 8);

                              calcPFCS(recbuf,56);

                              rec_len = 56;
                              write(recd[i].fd, "DVTOOL", 6);
                              write(recd[i].fd, &num_recs, 4);
                              write(recd[i].fd, &rec_len, 2);
                              write(recd[i].fd, (char *)recbuf, rec_len);
                           } 
                        }
                     }
                  }
                  else
                  /* check for cross-banding */
                  if ((memcmp(readBuffer + 36, "CQCQCQ", 6) == 0) && /* yrcall is CQCQCQ */
                      (memcmp(readBuffer + 28, OWNER, 7) == 0) &&    /* rpt1 is this repeater */
                      (memcmp(readBuffer + 20, OWNER, 7) == 0) &&    /* rpt2 is this repeater */
                      ((readBuffer[35] == 'A') ||
                       (readBuffer[35] == 'B') ||
                       (readBuffer[35] == 'C')) &&                   /* mod of rpt1 is A,B,C */
                      ((readBuffer[27] == 'A') ||
                       (readBuffer[27] == 'B') ||
                       (readBuffer[27] == 'C')) &&           /* !!! usually a G of rpt2, but we see A,B,C */
                      (readBuffer[35] != readBuffer[27]))    /* cross-banding? make sure NOT the same */
                  {
                     i = -1;
                     if (readBuffer[35] == 'A')
                        i = 0;
                     else
                     if (readBuffer[35] == 'B')
                        i = 1;
                     else
                     if (readBuffer[35] == 'C')
                        i = 2;

                     if (i >= 0)
                     {
                        /*
                           The remote repeater has been set, lets fill in the dest_rptr
                           so that later we can send that to the LIVE web site
                        */
                        memcpy(band_txt[i].dest_rptr, readBuffer + 20, CALL_SIZE);
                        band_txt[i].dest_rptr[CALL_SIZE] = '\0';
                     }

                     i = -1;
                     if (readBuffer[27] == 'A')
                        i = 0;
                     else
                     if (readBuffer[27] == 'B')
                        i = 1;
                     else
                     if (readBuffer[27] == 'C')
                        i = 2;

                     /* valid destination repeater module? */
                     if (i >= 0)
                     {
                        if ((toRptr_rp2c[i].last_time == 0) && (band_txt[i].last_time == 0))
                        {
                           traceit("ZONEmode cross-banding from mod %c to %c\n",  readBuffer[35], readBuffer[27]);

                           readBuffer[35] = 'G';
                           calcPFCS(readBuffer, 58);

                           sendto(srv_sock, (char *)readBuffer,58,0,
                                  (struct sockaddr *)&toRptr,
                                  sizeof(struct sockaddr_in));

                           /* This is the active streamid */
                           toRptr_rp2c[i].streamid[0] = readBuffer[14];
                           toRptr_rp2c[i].streamid[1] = readBuffer[15];
                           toRptr_rp2c[i].adr = fromRptr.sin_addr.s_addr;

                           /* time it, in case stream times out */
                           time(&toRptr_rp2c[i].last_time);

                           /* bump the G2 counter */
                           G2_COUNTER_OUT ++;
                        }
                     }
                  }
            }
            else
            {
               for (i = 0; i < 3; i++)
               {
                  if (memcmp(band_txt[i].streamID, readBuffer + 14, 2) == 0)
                  {
                     time(&band_txt[i].last_time);

                     if ((readBuffer[16] & 0x40) != 0)
                     {
                        if (dtmf_buf_count[i] > 0)
                        {
                           memset(DTMF_FILE, 0, sizeof(DTMF_FILE));
                           snprintf(DTMF_FILE, FILENAME_MAX, "%s/%d_mod_DTMF_NOTIFY", DTMF_DIR, i);
                           if (DTMF_DEBUG)
                              traceit("Saving dtmfs=[%s] into file: [%s]\n", dtmf_buf[i], DTMF_FILE);
                           dtmf_fp = fopen(DTMF_FILE, "w");
                           if (!dtmf_fp)
                              traceit("Failed to create dtmf file %s\n", DTMF_FILE);
                           else
                           {
                              fprintf(dtmf_fp, "%s\n%s", dtmf_buf[i], band_txt[i].lh_mycall);
                              fclose(dtmf_fp); dtmf_fp = NULL;
                           }
                           memset(dtmf_buf[i], 0, sizeof(dtmf_buf[i]));
                           dtmf_buf_count[i] = 0;
                           dtmf_counter[i] = 0;
                           dtmf_last_frame[i] = 0;
                        }

                        ii->sendHeardWithTXStats(band_txt[i].lh_mycall,
                                             band_txt[i].lh_sfx,
                                             (strstr(band_txt[i].lh_yrcall,"REF") == NULL)?band_txt[i].lh_yrcall:"CQCQCQ  ",
                                             band_txt[i].lh_rpt1,
                                             band_txt[i].lh_rpt2,
                                             band_txt[i].flags[0],
                                             band_txt[i].flags[1],
                                             band_txt[i].flags[2],
                                             band_txt[i].num_dv_frames,
                                             band_txt[i].num_dv_silent_frames,
                                             band_txt[i].num_bit_errors);

                        band_txt[i].streamID[0] = 0x00;
                        band_txt[i].streamID[1] = 0x00;
                        band_txt[i].flags[0] = 0x00;
                        band_txt[i].flags[1] = 0x00;
                        band_txt[i].flags[2] = 0x00;
                        band_txt[i].lh_mycall[0] = '\0';
                        band_txt[i].lh_sfx[0] = '\0';
                        band_txt[i].lh_yrcall[0] = '\0';
                        band_txt[i].lh_rpt1[0] = '\0';
                        band_txt[i].lh_rpt2[0] = '\0';

                        band_txt[i].last_time = 0;

                        band_txt[i].txt[0] = '\0';
                        band_txt[i].txt_cnt = 0;

                        band_txt[i].dest_rptr[0] = '\0';

                        band_txt[i].num_dv_frames = 0;
                        band_txt[i].num_dv_silent_frames = 0;
                        band_txt[i].num_bit_errors = 0;
                     }
                     else
                     {
                        ber_errs = dstar_dv_decode(readBuffer + 17, ber_data);
                        // traceit("errors = %d\n", ber_errs);
                        if (ber_data[0] == 0xf85)
                           band_txt[i].num_dv_silent_frames ++;
                        band_txt[i].num_bit_errors += ber_errs;
                        band_txt[i].num_dv_frames ++;

                        if ((ber_data[0] & 0x0ffc) == 0xfc0)
                        {
                           dtmf_digit = (ber_data[0] & 0x03) | ((ber_data[2] & 0x60) >> 3);
                           if (dtmf_counter[i] > 0)
                           {
                              if (dtmf_last_frame[i] != dtmf_digit)
                                 dtmf_counter[i] = 0;
                           }
                           dtmf_last_frame[i] = dtmf_digit;
                           dtmf_counter[i] ++;

                           if ((dtmf_counter[i] == 5) && (dtmf_digit >= 0) && (dtmf_digit <= 15))
                           {
                              if (dtmf_buf_count[i] < MAX_DTMF_BUF)
                              {
                                 dtmf_buf[i][ dtmf_buf_count[i] ] = dtmf_chars[dtmf_digit];
                                 dtmf_buf_count[i] ++;
                              }
                           }
                           memcpy(readBuffer + 17, silence, 9);
                        }
                        else
                           dtmf_counter[i] = 0;
                     }
                     break;
                  }
               }

               if (recvlen == 29)
                  memcpy(tmp_txt, readBuffer + 26, 3);
               else
                  memcpy(tmp_txt, readBuffer + 29, 3);

               // traceit("%x%x%x\n", tmp_txt[0], tmp_txt[1], tmp_txt[2]);
               // traceit("%c%c%c\n", tmp_txt[0] ^ 0x70, tmp_txt[1] ^ 0x4f, tmp_txt[2] ^ 0x93);

               /* extract 20-byte RADIO ID */
               if ((tmp_txt[0] != 0x55) || (tmp_txt[1] != 0x2d) || (tmp_txt[2] != 0x16))
               {
                  // traceit("%x%x%x\n", tmp_txt[0], tmp_txt[1], tmp_txt[2]);
                  // traceit("%c%c%c\n", tmp_txt[0] ^ 0x70, tmp_txt[1] ^ 0x4f, tmp_txt[2] ^ 0x93);

                  for (i = 0; i < 3; i++)
                  {
                     if (memcmp(band_txt[i].streamID, readBuffer + 14, 2) == 0)
                     {
                        if (new_group[i])
                        {
                           tmp_txt[0] = tmp_txt[0] ^ 0x70;
                           header_type = tmp_txt[0] & 0xf0;

                           if ((header_type == 0x50) ||  /* header  */
                               (header_type == 0xc0))    /* squelch */
                           {
                              new_group[i] = false;
                              to_print[i] = 0;
                              ABC_grp[i] = false;
                           }
                           else
                           if (header_type == 0x30)  /* GPS or GPS id or APRS */
                           {
                              new_group[i] = false;
                              to_print[i] = tmp_txt[0] & 0x0f;
                              ABC_grp[i] = false;
                              if (to_print[i] > 5)
                                 to_print[i] = 5;
                              else
                              if (to_print[i] < 1)
                                 to_print[i] = 1;

                              if ((to_print[i] > 1) && (to_print[i] <= 5))
                              {
                                 /* something went wrong? all bets are off */
                                 if (band_txt[i].temp_line_cnt > 200)
                                 {
                                    traceit("Reached the limit in the OLD gps mode\n");
                                    band_txt[i].temp_line[0] = '\0';
                                    band_txt[i].temp_line_cnt = 0;
                                 }

                                 /* fresh GPS string, re-initialize */
                                 if ((to_print[i] == 5) && ((tmp_txt[1] ^ 0x4f) == '$'))
                                 {
                                    band_txt[i].temp_line[0] = '\0';
                                    band_txt[i].temp_line_cnt = 0;
                                 }

                                 /* do not copy CR, NL */
                                 if (((tmp_txt[1] ^ 0x4f) != '\r') && ((tmp_txt[1] ^ 0x4f) != '\n'))
                                 {
                                    band_txt[i].temp_line[band_txt[i].temp_line_cnt] = tmp_txt[1] ^ 0x4f;
                                    band_txt[i].temp_line_cnt++;
                                 }
                                 if (((tmp_txt[2] ^ 0x93) != '\r') && ((tmp_txt[2] ^ 0x93) != '\n'))
                                 {
                                    band_txt[i].temp_line[band_txt[i].temp_line_cnt] = tmp_txt[2] ^ 0x93;
                                    band_txt[i].temp_line_cnt++;
                                 }

                                 if (((tmp_txt[1] ^ 0x4f) == '\r') || ((tmp_txt[2] ^ 0x93) == '\r'))
                                 {
                                    if (memcmp(band_txt[i].temp_line, "$GPRMC", 6) == 0)
                                    {
                                       memcpy(band_txt[i].gprmc, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
                                       band_txt[i].gprmc[band_txt[i].temp_line_cnt] = '\0';
                                    }
                                    else
                                    if (band_txt[i].temp_line[0] != '$')
                                    {
                                       memcpy(band_txt[i].gpid, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
                                       band_txt[i].gpid[band_txt[i].temp_line_cnt] = '\0';
                                       if (SEND_APRS && !band_txt[i].is_gps_sent)
                                          gps_send(i);
                                    }
                                    band_txt[i].temp_line[0] = '\0';
                                    band_txt[i].temp_line_cnt = 0;
                                 }
                                 else
                                 if (((tmp_txt[1] ^ 0x4f) == '\n') || ((tmp_txt[2] ^ 0x93) == '\n'))
                                 {
                                    band_txt[i].temp_line[0] = '\0';
                                    band_txt[i].temp_line_cnt = 0;
                                 }
                                 to_print[i] -= 2;
                              }
                              else
                              {
                                 /* something went wrong? all bets are off */
                                 if (band_txt[i].temp_line_cnt > 200)
                                 {
                                    traceit("Reached the limit in the OLD gps mode\n");
                                    band_txt[i].temp_line[0] = '\0';
                                    band_txt[i].temp_line_cnt = 0;
                                 }

                                 /* do not copy CR, NL */
                                 if (((tmp_txt[1] ^ 0x4f) != '\r') && ((tmp_txt[1] ^ 0x4f) != '\n'))
                                 {
                                    band_txt[i].temp_line[band_txt[i].temp_line_cnt] = tmp_txt[1] ^ 0x4f;
                                    band_txt[i].temp_line_cnt++;
                                 }

                                 if ((tmp_txt[1] ^ 0x4f) == '\r')
                                 {
                                    if (memcmp(band_txt[i].temp_line, "$GPRMC", 6) == 0)
                                    {
                                       memcpy(band_txt[i].gprmc, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
                                       band_txt[i].gprmc[band_txt[i].temp_line_cnt] = '\0';
                                    }
                                    else
                                    if (band_txt[i].temp_line[0] != '$')
                                    {
                                       memcpy(band_txt[i].gpid, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
                                       band_txt[i].gpid[band_txt[i].temp_line_cnt] = '\0';
                                       if (SEND_APRS && !band_txt[i].is_gps_sent)
                                          gps_send(i);
                                    }
                                    band_txt[i].temp_line[0] = '\0';
                                    band_txt[i].temp_line_cnt = 0;
                                 }
                                 else
                                 if ((tmp_txt[1] ^ 0x4f) == '\n')
                                 {
                                    band_txt[i].temp_line[0] = '\0';
                                    band_txt[i].temp_line_cnt = 0;
                                 }
                                 to_print[i] --;
                              }
                           }
                           else
                           if (header_type == 0x40) /* ABC text */
                           {
                              new_group[i] = false;
                              to_print[i] = 3;
                              ABC_grp[i] = true;
                              C_seen[i] = ((tmp_txt[0] & 0x0f) == 0x03)?true:false;

                              band_txt[i].txt[band_txt[i].txt_cnt] = tmp_txt[1] ^ 0x4f;
                              band_txt[i].txt_cnt++;

                              band_txt[i].txt[band_txt[i].txt_cnt] = tmp_txt[2] ^ 0x93;
                              band_txt[i].txt_cnt++;

                              /*
                                 We should NOT see any more text,
                                 if we already processed text,
                                 so blank out the codes.
                              */
                              if (band_txt[i].txt_stats_sent)
                              {
                                 if (recvlen == 29)
                                 {
                                    readBuffer[26] = 0x70;
                                    readBuffer[27] = 0x4f;
                                    readBuffer[28] = 0x93;
                                 }
                                 else
                                 {
                                    readBuffer[29] = 0x70;
                                    readBuffer[30] = 0x4f;
                                    readBuffer[31] = 0x93;
                                 }
                              }

                              if (band_txt[i].txt_cnt >= 20)
                              {
                                 band_txt[i].txt[band_txt[i].txt_cnt] = '\0';
                                 /***
                                 ii->sendHeardWithTXMsg(band_txt[i].lh_mycall,
                                                        band_txt[i].lh_sfx,
                                                        (strstr(band_txt[i].lh_yrcall,"REF") == NULL)?band_txt[i].lh_yrcall:"CQCQCQ  ",
                                                        band_txt[i].lh_rpt1,
                                                        band_txt[i].lh_rpt2,
                                                        band_txt[i].flags[0],
                                                        band_txt[i].flags[1],
                                                        band_txt[i].flags[2],
                                                        band_txt[i].dest_rptr,
                                                        band_txt[i].txt);
                                 ***/
                                 // traceit("TEXT1=[%s]\n", band_txt[i].txt);
                                 band_txt[i].txt_cnt = 0;
                              }
                           }
                           else
                           {
                              new_group[i] = false;
                              to_print[i] = 0;
                              ABC_grp[i] = false;
                           }
                        }
                        else
                        {
                           if (to_print[i] == 3)
                           {
                              if (ABC_grp[i])
                              {
                                 band_txt[i].txt[band_txt[i].txt_cnt] = tmp_txt[0] ^ 0x70;
                                 band_txt[i].txt_cnt ++;

                                 band_txt[i].txt[band_txt[i].txt_cnt] = tmp_txt[1] ^ 0x4f;
                                 band_txt[i].txt_cnt ++;

                                 band_txt[i].txt[band_txt[i].txt_cnt] = tmp_txt[2] ^ 0x93;
                                 band_txt[i].txt_cnt ++;

                                 /*
                                    We should NOT see any more text,
                                    if we already processed text,
                                    so blank out the codes.
                                 */
                                 if (band_txt[i].txt_stats_sent)
                                 {
                                    if (recvlen == 29)
                                    {
                                       readBuffer[26] = 0x70;
                                       readBuffer[27] = 0x4f;
                                       readBuffer[28] = 0x93;
                                    }
                                    else
                                    {
                                       readBuffer[29] = 0x70;
                                       readBuffer[30] = 0x4f;
                                       readBuffer[31] = 0x93;
                                    }
                                 }

                                 if ((band_txt[i].txt_cnt >= 20) || C_seen[i])
                                 {
                                    band_txt[i].txt[band_txt[i].txt_cnt] = '\0';
                                    if (!band_txt[i].txt_stats_sent)
                                    {
                                       /*** if YRCALL is CQCQCQ, set dest_rptr ***/
                                       if (memcmp(band_txt[i].lh_yrcall, "CQCQCQ", 6) == 0)
                                       {
                                          set_dest_rptr(i, band_txt[i].dest_rptr);
                                          if (memcmp(band_txt[i].dest_rptr, "REF", 3) == 0)
                                             band_txt[i].dest_rptr[0] = '\0';
                                       }

                                       ii->sendHeardWithTXMsg(band_txt[i].lh_mycall,
                                                        band_txt[i].lh_sfx,
                                                        (strstr(band_txt[i].lh_yrcall,"REF") == NULL)?band_txt[i].lh_yrcall:"CQCQCQ  ",
                                                        band_txt[i].lh_rpt1,
                                                        band_txt[i].lh_rpt2,
                                                        band_txt[i].flags[0],
                                                        band_txt[i].flags[1],
                                                        band_txt[i].flags[2],
                                                        band_txt[i].dest_rptr,
                                                        band_txt[i].txt);
                                       // traceit("TEXT2=[%s], destination repeater=[%s]\n", band_txt[i].txt, band_txt[i].dest_rptr);
                                       band_txt[i].txt_stats_sent = true;
                                    }
                                    band_txt[i].txt_cnt = 0;
                                 }
                                 if (C_seen[i])
                                    C_seen[i] = false;
                              }
                              else
                              {
                                 /* something went wrong? all bets are off */
                                 if (band_txt[i].temp_line_cnt > 200)
                                 {
                                    traceit("Reached the limit in the OLD gps mode\n");
                                    band_txt[i].temp_line[0] = '\0';
                                    band_txt[i].temp_line_cnt = 0;
                                 }

                                 /* do not copy CR, NL */
                                 if (((tmp_txt[0] ^ 0x70) != '\r') && ((tmp_txt[0] ^ 0x70) != '\n'))
                                 {
                                    band_txt[i].temp_line[band_txt[i].temp_line_cnt] = tmp_txt[0] ^ 0x70;
                                    band_txt[i].temp_line_cnt++;
                                 }
                                 if (((tmp_txt[1] ^ 0x4f) != '\r') && ((tmp_txt[1] ^ 0x4f) != '\n'))
                                 {
                                    band_txt[i].temp_line[band_txt[i].temp_line_cnt] = tmp_txt[1] ^ 0x4f;
                                    band_txt[i].temp_line_cnt++;
                                 }
                                 if (((tmp_txt[2] ^ 0x93) != '\r') && ((tmp_txt[2] ^ 0x93) != '\n'))
                                 {
                                    band_txt[i].temp_line[band_txt[i].temp_line_cnt] = tmp_txt[2] ^ 0x93;
                                    band_txt[i].temp_line_cnt++;
                                 }

                                 if (
                                      ((tmp_txt[0] ^ 0x70) == '\r') ||
                                      ((tmp_txt[1] ^ 0x4f) == '\r') ||
                                      ((tmp_txt[2] ^ 0x93) == '\r')
                                    )
                                 {
                                    if (memcmp(band_txt[i].temp_line, "$GPRMC", 6) == 0)
                                    {
                                       memcpy(band_txt[i].gprmc, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
                                       band_txt[i].gprmc[band_txt[i].temp_line_cnt] = '\0';
                                    }
                                    else
                                    if (band_txt[i].temp_line[0] != '$')
                                    {
                                       memcpy(band_txt[i].gpid, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
                                       band_txt[i].gpid[band_txt[i].temp_line_cnt] = '\0';
                                       if (SEND_APRS && !band_txt[i].is_gps_sent)
                                          gps_send(i);
                                    }
                                    band_txt[i].temp_line[0] = '\0';
                                    band_txt[i].temp_line_cnt = 0;
                                 }
                                 else
                                 if (((tmp_txt[0] ^ 0x70) == '\n') ||
                                     ((tmp_txt[1] ^ 0x4f) == '\n') ||
                                     ((tmp_txt[2] ^ 0x93) == '\n'))
                                 {
                                    band_txt[i].temp_line[0] = '\0';
                                    band_txt[i].temp_line_cnt = 0;
                                 }
                              }
                           }
                           else
                           if (to_print[i] == 2)
                           {
                              /* something went wrong? all bets are off */
                              if (band_txt[i].temp_line_cnt > 200)
                              {
                                 traceit("Reached the limit in the OLD gps mode\n");
                                 band_txt[i].temp_line[0] = '\0';
                                 band_txt[i].temp_line_cnt = 0;
                              }

                              /* do not copy CR, NL */
                              if (((tmp_txt[0] ^ 0x70) != '\r') && ((tmp_txt[0] ^ 0x70) != '\n'))
                              {
                                 band_txt[i].temp_line[band_txt[i].temp_line_cnt] = tmp_txt[0] ^ 0x70;
                                 band_txt[i].temp_line_cnt++;
                              }
                              if (((tmp_txt[1] ^ 0x4f) != '\r') && ((tmp_txt[1] ^ 0x4f) != '\n'))
                              {
                                 band_txt[i].temp_line[band_txt[i].temp_line_cnt] = tmp_txt[1] ^ 0x4f;
                                 band_txt[i].temp_line_cnt++;
                              }

                              if (((tmp_txt[0] ^ 0x70) == '\r') || ((tmp_txt[1] ^ 0x4f) == '\r'))
                              {
                                 if (memcmp(band_txt[i].temp_line, "$GPRMC", 6) == 0)
                                 {
                                    memcpy(band_txt[i].gprmc, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
                                    band_txt[i].gprmc[band_txt[i].temp_line_cnt] = '\0';
                                 }
                                 else
                                 if (band_txt[i].temp_line[0] != '$')
                                 {
                                    memcpy(band_txt[i].gpid, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
                                    band_txt[i].gpid[band_txt[i].temp_line_cnt] = '\0';
                                    if (SEND_APRS && !band_txt[i].is_gps_sent)
                                       gps_send(i);
                                 }
                                 band_txt[i].temp_line[0] = '\0';
                                 band_txt[i].temp_line_cnt = 0;
                              }
                              else
                              if (((tmp_txt[0] ^ 0x70) == '\n') || ((tmp_txt[1] ^ 0x4f)  == '\n'))
                              {
                                 band_txt[i].temp_line[0] = '\0';
                                 band_txt[i].temp_line_cnt = 0;
                              }
                           }
                           else
                           if (to_print[i] == 1)
                           {
                              /* something went wrong? all bets are off */
                              if (band_txt[i].temp_line_cnt > 200)
                              {
                                 traceit("Reached the limit in the OLD gps mode\n");
                                 band_txt[i].temp_line[0] = '\0';
                                 band_txt[i].temp_line_cnt = 0;
                              }

                              /* do not copy CR, NL */
                              if (((tmp_txt[0] ^ 0x70) != '\r') && ((tmp_txt[0] ^ 0x70) != '\n'))
                              {
                                 band_txt[i].temp_line[band_txt[i].temp_line_cnt] = tmp_txt[0] ^ 0x70;
                                 band_txt[i].temp_line_cnt++;
                              }

                              if ((tmp_txt[0] ^ 0x70) == '\r')
                              {
                                 if (memcmp(band_txt[i].temp_line, "$GPRMC", 6) == 0)
                                 {
                                    memcpy(band_txt[i].gprmc, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
                                    band_txt[i].gprmc[band_txt[i].temp_line_cnt] = '\0';
                                 }
                                 else
                                 if (band_txt[i].temp_line[0] != '$')
                                 {
                                    memcpy(band_txt[i].gpid, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
                                    band_txt[i].gpid[band_txt[i].temp_line_cnt] = '\0';
                                    if (SEND_APRS && !band_txt[i].is_gps_sent)
                                       gps_send(i);
                                 }
                                 band_txt[i].temp_line[0] = '\0';
                                 band_txt[i].temp_line_cnt = 0;
                              }
                              else
                              if ((tmp_txt[0] ^ 0x70) == '\n')
                              {
                                 band_txt[i].temp_line[0] = '\0';
                                 band_txt[i].temp_line_cnt = 0;
                              }
                           }
                           new_group[i] = true;
                           to_print[i] = 0;
                           ABC_grp[i] = false;
                        }
                        break;
                     }
                  }
               }

               /* send data to g2_link */
               sendto(g2_sock, (char *)readBuffer, recvlen,0,
                      (struct sockaddr *)&plug,
                      sizeof(struct sockaddr_in));

               /* aprs processing */
               if (SEND_APRS)
                  aprs_process_text(readBuffer + 14,          /* stream ID    */
                                    readBuffer[16],           /* seq          */
                                    readBuffer + 17,          /* audio + text */
                                    (recvlen == 29)?12:15);   /* size         */

               for (i = 0; i < 3; i++)
               {
                  /* find out if data must go to the remote G2 */
                  if (memcmp(to_remote_g2[i].streamid, readBuffer + 14, 2) == 0)
                  {
                     memcpy(readBuffer2, "DSVT", 4);
                     readBuffer2[4] = 0x20;
                     readBuffer2[5] = 0x00;
                     readBuffer2[6] = 0x00;
                     readBuffer2[7] = 0x00;
                     readBuffer2[8] =  readBuffer[10];
                     readBuffer2[9] =  readBuffer[11]; 
                     readBuffer2[10] = readBuffer[12]; 
                     readBuffer2[11] = readBuffer[13]; 
                     memcpy(readBuffer2 + 12, readBuffer + 14, 3);
                     if (recvlen == 29)
                        memcpy(readBuffer2 + 15, readBuffer + 17, 12);
                     else
                        memcpy(readBuffer2 + 15, readBuffer + 20, 12);

                     sendto(g2_sock, (char *)readBuffer2, 27,
                            0,(struct sockaddr *)&(to_remote_g2[i].toDst4),
                            sizeof(struct sockaddr_in));

                     time(&(to_remote_g2[i].last_time));

                     /* Is this the end-of-stream */
                     if ((readBuffer[16] & 0x40) != 0)
                     {
                        memset(&(to_remote_g2[i].toDst4),0,sizeof(struct sockaddr_in));
                        to_remote_g2[i].streamid[0] = '\0';
                        to_remote_g2[i].streamid[1] = '\0';
                        to_remote_g2[i].last_time = 0;
                     }
                     break;
                  }
                  else
                  /* Is the data to be recorded for echotest */
                  if ((recd[i].fd >= 0) &&
                      (memcmp(recd[i].streamid, readBuffer + 14, 2) == 0))
                  {
                     time(&recd[i].last_time);

                     memcpy(recbuf, "DSVT", 4);
                     recbuf[4] = 0x20;
                     recbuf[5] = 0x00;
                     recbuf[6] = 0x00;
                     recbuf[7] = 0x00;
                     recbuf[8] = readBuffer[10];
                     recbuf[9] = readBuffer[11];
                     recbuf[10] = readBuffer[12];
                     recbuf[11] = readBuffer[13];
                     memcpy(recbuf + 12, readBuffer + 14, 3);
                     if (recvlen == 29)
                        memcpy(recbuf + 15, readBuffer + 17, 12);
                     else
                        memcpy(recbuf + 15, readBuffer + 20, 12);

                     rec_len = 27;
                     write(recd[i].fd, &rec_len, 2);
                     write(recd[i].fd, (char *)recbuf, rec_len);

                     if ((readBuffer[16] & 0x40) != 0)
                     {
                        recd[i].streamid[0] = 0x00;
                        recd[i].streamid[1] = 0x00;
                        recd[i].last_time = 0;
                        close(recd[i].fd); recd[i].fd = -1;
                        // traceit("Closed echotest audio file:[%s]\n", recd[i].file);

                        /* we are in echotest mode, so play it back */
                        pthread_attr_init(&attr);
                        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                        rc = pthread_create(&echo_thread, &attr, echotest, (void *)recd[i].file);
                        if (rc != 0)
                        {
                           traceit("failed to start echotest thread\n");
                           /*
                              When the echotest thread runs, it deletes the file,
                              Because the echotest thread did NOT start, we delete the file here
                           */
                           unlink(recd[i].file);
                        }
                        pthread_attr_destroy(&attr);
                     }
                     break;
                  }
                  else
                  /* Is the data to be recorded for voicemail */
                  if ((vm[i].fd >= 0) &&
                      (memcmp(vm[i].streamid, readBuffer + 14, 2) == 0))
                  {
                     time(&vm[i].last_time);

                     memcpy(recbuf, "DSVT", 4);
                     recbuf[4] = 0x20;
                     recbuf[5] = 0x00;
                     recbuf[6] = 0x00;
                     recbuf[7] = 0x00;
                     recbuf[8] = readBuffer[10];
                     recbuf[9] = readBuffer[11];
                     recbuf[10] = readBuffer[12];
                     recbuf[11] = readBuffer[13];
                     memcpy(recbuf + 12, readBuffer + 14, 3);
                     if (recvlen == 29)
                        memcpy(recbuf + 15, readBuffer + 17, 12);
                     else
                        memcpy(recbuf + 15, readBuffer + 20, 12);

                     rec_len = 27;
                     write(vm[i].fd, &rec_len, 2);
                     write(vm[i].fd, (char *)recbuf, rec_len);

                     if ((readBuffer[16] & 0x40) != 0)
                     {
                        vm[i].streamid[0] = 0x00;
                        vm[i].streamid[1] = 0x00;
                        vm[i].last_time = 0;
                        close(vm[i].fd); vm[i].fd = -1;
                        // traceit("Closed voicemail audio file:[%s]\n", vm[i].file);
                     }
                     break;
                  }
                  else
                  /* or maybe this is cross-banding data */
                  if ((memcmp(toRptr_rp2c[i].streamid, readBuffer + 14, 2) == 0) &&
                      (toRptr_rp2c[i].adr == fromRptr.sin_addr.s_addr))
                  {
                     sendto(srv_sock, (char *)readBuffer,29,0,
                            (struct sockaddr *)&toRptr,
                            sizeof(struct sockaddr_in));

                     /* timeit */
                     time(&toRptr_rp2c[i].last_time);

                     /* bump G2 counter */
                     G2_COUNTER_OUT ++;

                     /* End of stream ? */
                     if ((readBuffer[16] & 0x40) != 0)
                     {
                        toRptr_rp2c[i].last_time = 0;
                        toRptr_rp2c[i].streamid[0] = '\0';
                        toRptr_rp2c[i].streamid[1] = '\0';
                        toRptr_rp2c[i].adr = 0;
                     }
                     break;
                  }
               }

               if ((readBuffer[16] & 0x40) != 0)
               {
                  if (QSO_DETAILS)
                     traceit("END from rptr: cntr=%02x %02x, streamID=%d,%d, %d bytes\n",
                             readBuffer[4], readBuffer[5],
                             readBuffer[14],readBuffer[15],recvlen);
               }
            }
         }
         FD_CLR (srv_sock,&fdset);
      }
   }
}

static void compute_aprs_hash()
{
   short hash = 0x73e2;
   short i = 0;
   short len = 0;
   char *p = NULL;
   char rptr_sign[CALL_SIZE + 1];
   
   strcpy(rptr_sign, OWNER);
   p = strchr(rptr_sign, ' ');
   if (!p)
   {
      traceit("Failed to build repeater callsign for aprs hash\n");
      return;
   }
   *p = '\0';
   p = rptr_sign;
   len = strlen(rptr_sign);
   
   while (i < len)
   {
      hash ^= (*p++) << 8;
      hash ^= (*p++);
      i += 2;
   }
   traceit("aprs hash code=[%d] for %s\n", hash, OWNER);
   RPTR_ID.aprs_hash = hash;

   return;
}  

static void aprs_open() 
{
   bool ok = false;
   struct timespec req;

   fd_set fdset;
   struct timeval tv;
   short int MAX_WAIT = 15; /* 15 seconds wait time MAX */
   int val = 1;
   socklen_t val_len;
   char snd_buf[512];
   char rcv_buf[512];
   int rc = 0;

   /* do some mimimal checking */
   if (RPTR_ID.aprs_host[0] == '\0')
   {
      traceit("Invalid value for APRS_HOST\n");
      return;
   }
   if (RPTR_ID.aprs_port == 0)
   {
      traceit("Invalid value for APRS_PORT\n");
      return;
   }
   if (RPTR_ID.aprs_interval == 0)
   {
      traceit("Invalid value for APRS_INTERVAL\n");
      return;
   }

   ok = resolve_rmt(RPTR_ID.aprs_host, SOCK_STREAM, &aprs_addr);
   if (!ok)
   {
      traceit("Can not resolve APRS_HOST %s\n", RPTR_ID.aprs_host);
      return;
   }

   /* fill it in */
   aprs_addr.sin_family = AF_INET;
   aprs_addr.sin_port = htons(RPTR_ID.aprs_port);

   aprs_addr_len = sizeof(aprs_addr);

   aprs_sock = socket(PF_INET,SOCK_STREAM,0);
   if (aprs_sock == -1)
   {
      traceit("Failed to create aprs socket,error=%d\n",errno);
      return;
   }
   fcntl(aprs_sock,F_SETFL,O_NONBLOCK);

   val = 1;
   if (setsockopt(aprs_sock,IPPROTO_TCP,TCP_NODELAY,(char *)&val, sizeof(val)) == -1)
   {
      traceit("setsockopt TCP_NODELAY TCP for aprs socket failed,error=%d\n",errno);
      close(aprs_sock);
      aprs_sock = -1;
      return;
   }

   traceit("Trying to connect to APRS...\n");
   rc = connect(aprs_sock, (struct sockaddr *)&aprs_addr, aprs_addr_len);
   if (rc != 0)
   {
      if (errno == EINPROGRESS)
      {
         traceit("Waiting for up to %d seconds for APRS_HOST\n", MAX_WAIT);
         while (MAX_WAIT > 0)
         {
            tv.tv_sec = 0;
            tv.tv_usec = 0;
            FD_ZERO(&fdset);
            FD_SET(aprs_sock, &fdset);
            rc = select(aprs_sock + 1, NULL,  &fdset, NULL, &tv);

            if (rc < 0)
            {
               traceit("Failed to connect to APRS...select,error=%d\n", errno);
               close(aprs_sock);
               aprs_sock = -1;
               return;
            }
            else
            if (rc == 0) /* timeout */
            {
               MAX_WAIT--;
               sleep(1);
            }
            else
            {
               val = 1; /* Assume it fails */
               val_len = sizeof(val);
               if (getsockopt(aprs_sock, SOL_SOCKET, SO_ERROR, (char *) &val, &val_len) < 0)
               {
                  traceit("Failed to connect to APRS...getsockopt, error=%d\n", errno);
                  close(aprs_sock);
                  aprs_sock = -1;
                  return;
               }
               else
               if (val == 0)
                  break;

               MAX_WAIT--;
               sleep(1);
            }
         }
         if (MAX_WAIT == 0)
         {
            traceit("Failed to connect to APRS...timeout\n");
            close(aprs_sock);
            aprs_sock = -1;
            return;
         }
      }
      else
      {
         traceit("Failed to connect to APRS, error=%d\n", errno);
         close(aprs_sock);
         aprs_sock = -1;
         return;
      }
   }
   traceit("Connected to APRS %s:%d\n", RPTR_ID.aprs_host, RPTR_ID.aprs_port);

   /* login to aprs */
   sprintf(snd_buf, "user %s pass %d vers g2_ircddb 2.99 UDP 5 ",
           OWNER, RPTR_ID.aprs_hash);

   /* add the user's filter */
   if (RPTR_ID.aprs_filter[0] != '\0')
   {
      strcat(snd_buf, "filter ");
      strcat(snd_buf, RPTR_ID.aprs_filter);
   }
   // traceit("APRS login command:[%s]\n", snd_buf);
   strcat(snd_buf, "\r\n");

   while (true)
   {
      rc = writen(snd_buf, strlen(snd_buf));
      if (rc < 0)
      {
         if (errno == EWOULDBLOCK)
         {
            recv(aprs_sock, rcv_buf, sizeof(rcv_buf), 0);
            req.tv_sec = 0;
            req.tv_nsec = 100000000; // 100 milli
            nanosleep(&req, NULL);
         }
         else
         {
            traceit("APRS login command failed, error=%d\n", errno);
            break;
         }
      }
      else
      {
         // traceit("APRS login command sent\n");
         break;
      }
   }
   recv(aprs_sock, rcv_buf, sizeof(rcv_buf), 0);

   return;
}

void *send_aprs_beacon(void *arg)
{
   struct timespec req;

   int rc;
   char snd_buf[512];
   char rcv_buf[512];
   float tmp_lat;
   float tmp_lon;
   float lat;
   float lon;
   char lat_s[15];
   char lon_s[15];
   time_t last_beacon_time = 0;
   time_t last_keepalive_time = 0;
   time_t tnow = 0;
   short int i;
   struct sigaction act;

   /*
      Every 20 seconds, the remote APRS host sends a KEEPALIVE packet-comment
      on the TCP/APRS port.
      If we have not received any KEEPALIVE packet-comment after 5 minutes
      we must assume that the remote APRS host is down or disappeared
      or has dropped the connection. In these cases, we must re-connect.
      There are 3 keepalive packets in one minute, or every 20 seconds.
      In 5 minutes, we should have received a total of 15 keepalive packets.
   */
   short THRESHOLD_COUNTDOWN = 15;

   arg = arg;

   act.sa_handler = sigCatch;
   sigemptyset(&act.sa_mask);
   act.sa_flags = SA_RESTART;
   if (sigaction(SIGTERM, &act, 0) != 0)
   {
      traceit("sigaction-TERM failed, error=%d\n", errno);
      traceit("beacon thread exiting...\n");
      pthread_exit(NULL);
   }
   if (sigaction(SIGINT, &act, 0) != 0)
   {
      traceit("sigaction-INT failed, error=%d\n", errno);
      traceit("beacon thread exiting...\n");
      pthread_exit(NULL);
   }
   if (sigaction(SIGPIPE, &act, 0) != 0)
   {
      traceit("sigaction-PIPE failed, error=%d\n", errno);
      traceit("beacon thread exiting...\n");
      pthread_exit(NULL);
   }

   time(&last_keepalive_time);

   /* This thread is also saying to the APRS_HOST that we are ALIVE */
   while (keep_running)
   {
      if (aprs_sock == -1)
      {
         aprs_open();
         if (aprs_sock == -1)
            sleep(1);
         else
            THRESHOLD_COUNTDOWN = 15;
      }

      time(&tnow);
      if ((tnow - last_beacon_time) > (RPTR_ID.aprs_interval * 60))
      {
         for (i = 0; i < 3; i++)
         {
            if (RPTR_ID.desc[i][0] != '\0')
            {
               tmp_lat = fabs(RPTR_ID.lat[i]);
               tmp_lon = fabs(RPTR_ID.lon[i]);
               lat = floor(tmp_lat);
               lon = floor(tmp_lon);
               lat = (tmp_lat - lat) * 60.0F + lat  * 100.0F;
               lon = (tmp_lon - lon) * 60.0F + lon  * 100.0F;

               if (lat >= 1000.0F)
                  sprintf(lat_s, "%.2f", lat);
               else
               if (lat >= 100.0F)
                  sprintf(lat_s, "0%.2f", lat);
               else
               if (lat >= 10.0F)
                  sprintf(lat_s, "00%.2f", lat);
               else
                  sprintf(lat_s, "000%.2f", lat);

               if (lon >= 10000.0F)
                  sprintf(lon_s, "%.2f", lon);
               else
               if (lon >= 1000.0F)
                  sprintf(lon_s, "0%.2f", lon);
               else
               if (lon >= 100.0F)
                  sprintf(lon_s, "00%.2f", lon);
               else
               if (lon >= 10.0F)
                  sprintf(lon_s, "000%.2f", lon);
               else
                  sprintf(lon_s, "0000%.2f", lon);

               /* send to aprs */
               sprintf(snd_buf, "%s>APJI23,TCPIP*,qAC,%sS:!%s%cD%s%c&RNG%04u %s %s",
                       RPTR_ID.rptr[i],  RPTR_ID.rptr[i],
                       lat_s,  (RPTR_ID.lat[i] < 0.0F)  ? 'S' : 'N',
                       lon_s,  (RPTR_ID.lon[i] < 0.0F) ? 'W' : 'E',
                       RPTR_ID.range[i], RPTR_ID.band[i], RPTR_ID.desc[i]);

               // traceit("APRS Beacon =[%s]\n", snd_buf);
               strcat(snd_buf, "\r\n");

               while (keep_running)
               {
                  if (aprs_sock == -1)
                  {
                     aprs_open();
                     if (aprs_sock == -1)
                        sleep(1);
                     else
                        THRESHOLD_COUNTDOWN = 15;
                  }
                  else
                  {
                     rc = writen(snd_buf, strlen(snd_buf));
                     if (rc < 0)
                     {
                        if ((errno == EPIPE) ||
                            (errno == ECONNRESET) ||
                            (errno == ETIMEDOUT) ||
                            (errno == ECONNABORTED) ||
                            (errno == ESHUTDOWN) ||
                            (errno == EHOSTUNREACH) ||
                            (errno == ENETRESET) ||
                            (errno == ENETDOWN) ||
                            (errno == ENETUNREACH) ||
                            (errno == EHOSTDOWN) ||
                            (errno == ENOTCONN))
                        {
                           traceit("send_aprs_beacon: APRS_HOST closed connection,error=%d\n",errno);
                           close(aprs_sock);
                           aprs_sock = -1;
                        }
                        else
                        if (errno == EWOULDBLOCK)
                        {
                           req.tv_sec = 0;
                           req.tv_nsec = 100000000; // 100 milli
                           nanosleep(&req, NULL);
                        }
                        else
                        {
                           /* Cant do nothing about it */
                           traceit("send_aprs_beacon failed, error=%d\n", errno);
                           break;
                        }
                     }
                     else
                     {
                        // traceit("APRS beacon sent\n");
                        break;
                     }
                  }
                  rc = recv(aprs_sock, rcv_buf, sizeof(rcv_buf), 0);
                  if (rc > 0)
                     THRESHOLD_COUNTDOWN = 15;
               }
            }
            rc = recv(aprs_sock, rcv_buf, sizeof(rcv_buf), 0);
            if (rc > 0)
               THRESHOLD_COUNTDOWN = 15;
         }
         time(&last_beacon_time);
      }
      /*
         Are we still receiving from APRS host ?
      */
      rc = recv(aprs_sock, rcv_buf, sizeof(rcv_buf), 0);
      if (rc < 0)
      {
         if ((errno == EPIPE) ||
             (errno == ECONNRESET) ||
             (errno == ETIMEDOUT) ||
             (errno == ECONNABORTED) ||
             (errno == ESHUTDOWN) ||
             (errno == EHOSTUNREACH) ||
             (errno == ENETRESET) ||
             (errno == ENETDOWN) ||
             (errno == ENETUNREACH) ||
             (errno == EHOSTDOWN) ||
             (errno == ENOTCONN))
         {
            traceit("send_aprs_beacon: recv error: APRS_HOST closed connection,error=%d\n",errno);
            close(aprs_sock);
            aprs_sock = -1;
         }
      }
      else
      if (rc == 0)
      {
         traceit("send_aprs_beacon: recv: APRS shutdown\n");
         close(aprs_sock);
         aprs_sock = -1;
      }
      else
         THRESHOLD_COUNTDOWN = 15;

      req.tv_sec = 0;
      req.tv_nsec = 100000000; // 100 milli
      nanosleep(&req, NULL);

      /* 20 seconds passed already ? */
      time(&tnow);
      if ((tnow - last_keepalive_time) > 20)
      {
         /* we should be receiving keepalive packets ONLY if the connection is alive */
         if (aprs_sock >= 0)
         {
            if (THRESHOLD_COUNTDOWN > 0)
               THRESHOLD_COUNTDOWN--;

            if (THRESHOLD_COUNTDOWN == 0)
            {
               traceit("APRS host keepalive timeout\n");
               close(aprs_sock);
               aprs_sock = -1;
            }
         }
         /* reset timer */
         time(&last_keepalive_time);
      }
   }
   traceit("beacon thread exiting...\n");
   pthread_exit(NULL);
}

void *echotest(void *arg)
{
   char *file = (char *)arg;
   struct timespec req;

   FILE *fp = NULL;
   unsigned short rlen = 0;
   size_t nread = 0;
   unsigned char dstar_buf[56];
   unsigned char rptr_buf[58];
   struct sigaction act;

   act.sa_handler = sigCatch;
   sigemptyset(&act.sa_mask);
   act.sa_flags = SA_RESTART;
   if (sigaction(SIGTERM, &act, 0) != 0)
   {
      traceit("sigaction-TERM failed, error=%d\n", errno);
      traceit("echotest thread exiting...\n");
      pthread_exit(NULL);
   }
   if (sigaction(SIGINT, &act, 0) != 0)
   {
      traceit("sigaction-INT failed, error=%d\n", errno);
      traceit("echotest thread exiting...\n");
      pthread_exit(NULL);
   }
   if (sigaction(SIGPIPE, &act, 0) != 0)
   {
      traceit("sigaction-PIPE failed, error=%d\n", errno);
      traceit("echotest thread exiting...\n");
      pthread_exit(NULL);
   }

   traceit("File to playback:[%s]\n", file);

   fp = fopen(file, "rb");
   if (!fp)
   {
      traceit("Failed to open file %s\n", file);
      pthread_exit(NULL);
   }

   nread = fread(dstar_buf, 10, 1, fp);
   if (nread != 1)
   {
      traceit("Cant read first 10 bytes in %s\n", file);
      fclose(fp);
      pthread_exit(NULL);
   }

   if (memcmp(dstar_buf, "DVTOOL", 6) != 0)
   {
      traceit("DVTOOL keyword not found in %s\n", file);
      fclose(fp);
      pthread_exit(NULL);
   }

   sleep(PLAY_WAIT);
   while (keep_running)
   {
      nread = fread(&rlen, 2, 1, fp);
      if (nread != 1)
         break;

      if ((rlen != 56) && (rlen != 27))
      {
         traceit("Expected 56 bytes or 27 bytes, found %d\n", rlen);
         break;
      }
      nread = fread(dstar_buf, rlen, 1, fp);
      if (nread == 1)
      {
         if (memcmp(dstar_buf, "DSVT", 4) != 0)
         {
            traceit("DVST keyword not found in %s\n", file);
            break;
         }

         if (dstar_buf[8] != 0x20)
         {
            traceit("Not Voice type in %s\n", file);
            break;
         }

         if ((dstar_buf[4] != 0x10) && (dstar_buf[4] != 0x20))
         {
            traceit("Not a valid record type in %s\n",file);
            break;
         }

         /***
            WARNING: G2_COUNTER_OUT is accessed by both threads.
            It should be protected with a MUTEX,
            Accessing G2_COUNTER_OUT and adding 1 to it,
              should be an atomic operation.
            The main thread will correct the G2_COUNTER_OUT
            after it receives a REPLY from RP2C,
            but still we should do the right thing and
            protect G2_COUNTER_OUT with a MUTEX.
         ***/

         if (rlen == 56)
         {
            memcpy(rptr_buf, "DSTR", 4);
            rptr_buf[5] = (unsigned char)(G2_COUNTER_OUT & 0xff);
            rptr_buf[4] = (unsigned char)((G2_COUNTER_OUT >> 8) & 0xff);
            rptr_buf[6] = 0x73;
            rptr_buf[7] = 0x12;
            rptr_buf[8] = 0x00;
            rptr_buf[9] = 0x30;
            rptr_buf[10] = 0x20;
            memcpy(rptr_buf + 11, dstar_buf + 9, 47);

            /* We did not change anything */
            // calcPFCS(rptr_buf, 58);
         }
         else
         {
            rptr_buf[5] = (unsigned char)(G2_COUNTER_OUT & 0xff);
            rptr_buf[4] = (unsigned char)((G2_COUNTER_OUT >> 8) & 0xff);
            rptr_buf[9] = 0x13;
            memcpy(rptr_buf + 11, dstar_buf + 9, 18);
         }
         
         sendto(srv_sock, (char *)rptr_buf, rlen + 2, 0,
                (struct sockaddr *)&toRptr,
                sizeof(struct sockaddr_in));

         G2_COUNTER_OUT ++;

         req.tv_sec = 0;
         req.tv_nsec = PLAY_DELAY * 1000000;
         nanosleep(&req, NULL);
      }
   }
   fclose(fp);
   if (!strstr(file, "voicemail.dat"))
      unlink(file);
   traceit("Finished playing\n");
   pthread_exit(NULL);
}

static void qrgs_and_maps()
{
   const char *delim = ",";

   char *lat_s = NULL;
   double lat = 0.00;
   char *lon_s = NULL;
   double lon = 0.00;
   char *desc1 = NULL;
   char *desc2 = NULL;
   char *url = NULL;

   char *freq_s = NULL;
   double freq = 0.00;
   char *shift_s = NULL;
   double shift = 0.00;
   char *range_s = NULL;
   double range = 0.00;
   char *agl_s = NULL;
   double agl = 0.00;

   char *saveptr = NULL;

   if (QTH[0] != '\0')
   {
      lat_s = strtok_r(QTH, delim, &saveptr);
      lon_s = strtok_r(NULL,  delim, &saveptr);
      desc1 = strtok_r(NULL,  delim, &saveptr);
      desc2 = strtok_r(NULL,  delim, &saveptr);
      url =   strtok_r(NULL,  delim, &saveptr);

      if (lat_s)
         lat = atof(lat_s);
      if (lon_s)
         lon = atof(lon_s);

      ii->rptrQTH(lat, lon, (desc1)?desc1:"", (desc2)?desc2:"", (url)?url:"");
   }

   saveptr = NULL;

   if (QRG_A[0] != '\0')
   {
      freq_s = strtok_r(QRG_A, delim, &saveptr);
      shift_s = strtok_r(NULL,  delim, &saveptr);
      range_s = strtok_r(NULL,  delim, &saveptr);
      agl_s = strtok_r(NULL,  delim, &saveptr);

      if (freq_s)
         freq = atof(freq_s);
      if (shift_s)
         shift = atof(shift_s);
      if (range_s)
         range = atof(range_s);
      if (agl_s)
         agl = atof(agl_s);

      ii->rptrQRG("A", freq, shift, range, agl);
   }

   saveptr = NULL;

   if (QRG_B[0] != '\0')
   {
      freq_s = strtok_r(QRG_B, delim, &saveptr);
      shift_s = strtok_r(NULL,  delim, &saveptr);
      range_s = strtok_r(NULL,  delim, &saveptr);
      agl_s = strtok_r(NULL,  delim, &saveptr);

      if (freq_s)
         freq = atof(freq_s);
      if (shift_s)
         shift = atof(shift_s);
      if (range_s)
         range = atof(range_s);
      if (agl_s)
         agl = atof(agl_s);

      ii->rptrQRG("B", freq, shift, range, agl);
   }

   saveptr = NULL;

   if (QRG_C[0] != '\0')
   {
      freq_s = strtok_r(QRG_C, delim, &saveptr);
      shift_s = strtok_r(NULL,  delim, &saveptr);
      range_s = strtok_r(NULL,  delim, &saveptr);
      agl_s = strtok_r(NULL,  delim, &saveptr);

      if (freq_s)
         freq = atof(freq_s);
      if (shift_s)
         shift = atof(shift_s);
      if (range_s)
         range = atof(range_s);
      if (agl_s)
         agl = atof(agl_s);

      ii->rptrQRG("C", freq, shift, range, agl);
   }

   return;
}

int main(int argc, char **argv)
{
   short int i;
   struct sigaction act;
   bool ok;
   char *p = NULL;

   int rc = 0;

   setvbuf(stdout, (char *)NULL, _IOLBF, 0);
   traceit("VERSION %s\n", VERSION);
   if (argc != 2)
   {
      traceit("Example: g2_ircddb g2_ircddb.cfg\n");
      return 1;
   }

   /* Used to validate MYCALL input */
   rc = regcomp(&preg, "^(([1-9][A-Z])|([A-Z][0-9])|([A-Z][A-Z][0-9]))[0-9A-Z]*[A-Z][ ]*[ A-RT-Z]$",
               REG_EXTENDED | REG_NOSUB);
   if (rc != REG_NOERROR)
   {
      traceit("The IRC regular expression is NOT valid\n");
      return 1;
   }

   act.sa_handler = sigCatch;
   sigemptyset(&act.sa_mask); 
   act.sa_flags = SA_RESTART;
   if (sigaction(SIGTERM, &act, 0) != 0)
   {
      traceit("sigaction-TERM failed, error=%d\n", errno);
      return 1;
   }
   if (sigaction(SIGINT, &act, 0) != 0)
   {
      traceit("sigaction-INT failed, error=%d\n", errno); 
      return 1;
   }
   if (sigaction(SIGPIPE, &act, 0) != 0)
   {
      traceit("sigaction-PIPE failed, error=%d\n", errno);
      return 1;
   }

   RPTR_ID.aprs_host[0] = '\0';
   RPTR_ID.aprs_port = 0;
   RPTR_ID.aprs_hash = -1;
   RPTR_ID.aprs_interval = 0;
   RPTR_ID.aprs_filter[0] = '\0';
   for (i = 0; i < 3; i++)
   {
      RPTR_ID.lat[i] = 0.00;
      RPTR_ID.lon[i] = 0.00;
      RPTR_ID.range[i] = 0;
      RPTR_ID.desc[i][0] = '\0';
   }
   strcpy(RPTR_ID.band[0], "23cm");
   strcpy(RPTR_ID.band[1], "70cm");
   strcpy(RPTR_ID.band[2], "2m");

   for (i = 0; i < 3; i++)
   {
      aprs_streamID[i].streamID[0] = 0x00;
      aprs_streamID[i].streamID[1] = 0x00;
      aprs_streamID[i].last_time = 0;
   }

   for (i = 0; i < 3; i++)
   {
      band_txt[i].streamID[0] = 0x00;
      band_txt[i].streamID[1] = 0x00;
      band_txt[i].flags[0] = 0x00;
      band_txt[i].flags[1] = 0x00;
      band_txt[i].flags[2] = 0x00;
      band_txt[i].lh_mycall[0] = '\0';
      band_txt[i].lh_sfx[0] = '\0';
      band_txt[i].lh_yrcall[0] = '\0';
      band_txt[i].lh_rpt1[0] = '\0';
      band_txt[i].lh_rpt2[0] = '\0';

      band_txt[i].last_time = 0;

      band_txt[i].txt[0] = '\0';
      band_txt[i].txt_cnt = 0;
      band_txt[i].txt_stats_sent = false;

      band_txt[i].dest_rptr[0] = '\0';

      band_txt[i].temp_line[0] = '\0';
      band_txt[i].temp_line_cnt = 0;
      band_txt[i].gprmc[0] = '\0';
      band_txt[i].gpid[0] = '\0';
      band_txt[i].is_gps_sent = false;
      band_txt[i].gps_last_time = 0;

      band_txt[i].num_dv_frames = 0;
      band_txt[i].num_dv_silent_frames = 0;
      band_txt[i].num_bit_errors = 0;

   }

   /* process configuration file */
   rc = read_config(argv[1]);
   if (rc != 0)
   {
      traceit("Failed to process config file %s\n", argv[1]);
      return 1;
   }

   /* build the repeater callsigns for aprs */
   strcpy(RPTR_ID.rptr[0], OWNER);
   p = strchr(RPTR_ID.rptr[0], ' ');
   if (!p)
   {
      traceit("Failed to build repeater callsigns for aprs\n");
      return 1;
   }
   *p = '\0';
   strcpy(RPTR_ID.rptr[1], RPTR_ID.rptr[0]);
   strcpy(RPTR_ID.rptr[2], RPTR_ID.rptr[0]);
   strcat(RPTR_ID.rptr[0], "-A");
   strcat(RPTR_ID.rptr[1], "-B");
   strcat(RPTR_ID.rptr[2], "-C");
   traceit("Repeater callsigns: [%s] [%s] [%s]\n",
            RPTR_ID.rptr[0], RPTR_ID.rptr[1], RPTR_ID.rptr[2]);

   aprs_init();
   compute_aprs_hash();

   ok =  ::wxInitialize();
   if (!ok)
   {
      traceit("Failed to initialize wx\n");
      return 1;
   }
   
   ii = new CIRCDDB(IRC_DDB_HOST, IRC_DDB_PORT, owner, IRC_PASS, VERSION, LOCAL_IRC_IP);
   ok = ii->open();
   if (!ok)
   {
      traceit("irc open failed\n");
      ::wxUninitialize();
      return 1;
   }

   rc = ii->getConnectionState();
   i = 0;
   traceit("Waiting for irc connection status of 2\n");
   while (rc < 2)
   {
      traceit("irc status=%d\n", rc);
      if (rc < 2)
      {
         i++;
         sleep(5);
      }
      else
         break;

      if (!keep_running)
         break;

      if (i > 5)
      {
         traceit("We can not wait any longer...\n");
         break;
      }
      rc = ii->getConnectionState();
   }

   do
   {
      /* udp port 40000 must open first */
      rc = g2_open();
      if (rc != 0)
      {
         traceit("g2_open() failed\n");
         break;
      }
  
      /* Open udp INTERNAL port */
      rc = srv_open();
      if (rc != 0)
      {
         traceit("srv_open() failed\n");
         break;
      }

      /* recording for echotest on local repeater modules */
      for (i = 0; i < 3; i++)
      {
         recd[i].last_time = 0;
         recd[i].streamid[0] = 0x00;
         recd[i].streamid[1] = 0x00;
         recd[i].fd = -1;
         memset(recd[i].file, 0, sizeof(recd[i].file));
      }

      /* recording for voicemail on local repeater modules */
      for (i = 0; i < 3; i++)
      {
         vm[i].last_time = 0;
         vm[i].streamid[0] = 0x00;
         vm[i].streamid[1] = 0x00;
         vm[i].fd = -1;
         memset(vm[i].file, 0, sizeof(vm[i].file));

         if (i == 0)
            snprintf(vm[i].file, FILENAME_MAX, "%s/%c_%s",
                     ECHOTEST_DIR,'A',"voicemail.dat");
         else
         if (i == 1)
            snprintf(vm[i].file, FILENAME_MAX, "%s/%c_%s",
                     ECHOTEST_DIR,'B',"voicemail.dat");
         else
            snprintf(vm[i].file, FILENAME_MAX, "%s/%c_%s",
                     ECHOTEST_DIR,'C',"voicemail.dat");

         if (access(vm[i].file, F_OK) != 0)
            memset(vm[i].file, 0, sizeof(vm[i].file));
         else
            traceit("Loaded voicemail file: %s for mod %d\n",
                    vm[i].file, i);
      }

      /* the repeater runs on this port */
      memset(&toRptr,0,sizeof(struct sockaddr_in));
      toRptr.sin_family = AF_INET;
      toRptr.sin_port = htons(TO_RPTR_PORT);
      toRptr.sin_addr.s_addr = inet_addr(TO_RPTR_IP);

      for (i = 0; i < 3; i++)
      {
         memset(&toRptr_rp2c[i],0,sizeof(toRptr_rp2c[i]));

         memset(toRptr_rp2c[i].saved_hdr, 0, sizeof(toRptr_rp2c[i].saved_hdr));
         toRptr_rp2c[i].saved_adr = 0;

         toRptr_rp2c[i].streamid[0] = '\0';
         toRptr_rp2c[i].streamid[1] = '\0';
         toRptr_rp2c[i].adr = 0;
         toRptr_rp2c[i].last_time = 0;
      }

      /* to remote systems */
      for (i = 0; i < 3; i++)
      {
         memset(&(to_remote_g2[i].toDst4),0,sizeof(struct sockaddr_in));
         to_remote_g2[i].streamid[0] = '\0';
         to_remote_g2[i].streamid[1] = '\0';
         to_remote_g2[i].last_time = 0;
      }

      /* where to send packets to g2_link */
      memset(&plug,0,sizeof(struct sockaddr_in));
      plug.sin_family = AF_INET;
      plug.sin_port = htons(TO_G2_LINK_PORT);
      plug.sin_addr.s_addr = inet_addr(TO_G2_LINK_IP);    

      traceit("g2_ircddb_rp2c...entering processing loop\n");

      if (SEND_QRGS_MAPS)
         qrgs_and_maps();

      runit();
      traceit("Leaving processing loop...\n"); 

   } while (false);

   if (srv_sock != -1)
   {
      close(srv_sock);
      traceit("Closed G2_INTERNAL_PORT\n");
   }

   if (g2_sock != -1)
   {
      close(g2_sock);
      traceit("Closed G2_EXTERNAL_PORT\n");
   }

   if (aprs_sock != -1)
   {
      close(aprs_sock);
      traceit("Closed APRS\n");
   }

   for (i = 0; i < 3; i++)
   {
      recd[i].last_time = 0;
      recd[i].streamid[0] = 0x00;
      recd[i].streamid[1] = 0x00;
      if (recd[i].fd >= 0)
      {
         close(recd[i].fd);
         unlink(recd[i].file);
      }
   }

   ii->close();
   ::wxUninitialize();
   traceit("g2_ircddb_rp2c exiting\n");
   return rc;
}

static void aprs_init()
{
   short int rptr_idx;

   /* Initialize the statistics on the APRS packets */
   for (rptr_idx = 0; rptr_idx < 3; rptr_idx++)
   {
      aprs_pack[rptr_idx].al = al_none;
      aprs_pack[rptr_idx].data[0] = '\0';
      aprs_pack[rptr_idx].len = 0;
      aprs_pack[rptr_idx].buf[0] = '\0';
      aprs_pack[rptr_idx].sl = sl_first;
      aprs_pack[rptr_idx].is_sent = false;
   }

   /* Initialize the APRS host */
   memset(&aprs_addr,0,sizeof(struct sockaddr_in));
   aprs_addr_len = sizeof(aprs_addr);

   return;
}

// This is called when header comes in from repeater
static void aprs_select_band(short int rptr_idx, unsigned char *streamID)
{
   if ((rptr_idx < 0) || (rptr_idx > 2))
   {
       traceit("ERROR in aprs_select_band, invalid mod %d\n", rptr_idx);
       return;
   }

   /* lock on the streamID */
   aprs_streamID[rptr_idx].streamID[0] = streamID[0];
   aprs_streamID[rptr_idx].streamID[1] = streamID[1];
   // aprs_streamID[rptr_idx].last_time = 0;

   aprs_reset(rptr_idx);
   return;
}

// This is called when data(text) comes in from repeater
// Parameter buf is either:
//              12 bytes(packet from repeater was 29 bytes) or
//              15 bytes(packet from repeater was 32 bytes)
// Parameter len is either 12 or 15, because we took passed over the first 17 bytes
//           in the repeater data
// Paramter seq is the byte at pos# 16(counting from zero) in the repeater data
static void aprs_process_text(unsigned char *streamID,
                              unsigned char seq,
                              unsigned char *buf,
                              unsigned int len)
{
   bool done = false;
   unsigned char aprs_data[200];
   unsigned int aprs_len;
   char *p = NULL;
   char *hdr = NULL;
   char *aud = NULL;
   char aprs_buf[1024];
   int rc;
   time_t tnow = 0;

   short int i;
   short int rptr_idx = -1;

   len = len;

   for (i = 0; i < 3; i++)
   {
      if (memcmp(streamID, aprs_streamID[i].streamID, 2) == 0)
      {
         rptr_idx = i;
         break;
      }
   }
   if ((rptr_idx < 0) || (rptr_idx > 2))
   {
      // traceit("ERROR in aprs_process_text: rptr_idx %d is invalid\n", rptr_idx);
      return;
   }

   if ((seq & 0x40) == 0x40)
      return;

   if ((seq & 0x1f) == 0x00)
   {
      aprs_sync_it(rptr_idx);
      return;
   }

   done = aprs_write_data(rptr_idx, buf + 9);
   if (!done)
      return;

   aprs_len = aprs_get_data(rptr_idx, aprs_data, 200);
   aprs_data[aprs_len] = '\0';

   time(&tnow);
   if ((tnow - aprs_streamID[rptr_idx].last_time) < 30)
      return;

   if (aprs_sock == -1)
      return;

   p = strchr((char*)aprs_data, ':');
   if (!p)
   {
      aprs_reset(rptr_idx);
      return;
   }
   *p = '\0';


   hdr = (char *)aprs_data;
   aud = p + 1;
   if (strchr(hdr, 'q') != NULL)
      return;

   p = strchr(aud, '\r');
   *p = '\0';

   sprintf(aprs_buf, "%s,qAR,%s:%s\r\n", hdr, RPTR_ID.rptr[rptr_idx], aud);
   // traceit("GPS-A=%s", aprs_buf);
   rc = writen(aprs_buf, strlen(aprs_buf));
   if (rc == -1)
   {
      if ((errno == EPIPE) ||
          (errno == ECONNRESET) ||
          (errno == ETIMEDOUT) ||
          (errno == ECONNABORTED) ||
          (errno == ESHUTDOWN) ||
          (errno == EHOSTUNREACH) ||
          (errno == ENETRESET) ||
          (errno == ENETDOWN) ||
          (errno == ENETUNREACH) ||
          (errno == EHOSTDOWN) ||
          (errno == ENOTCONN))
      {
         traceit("aprs_process_text: APRS_HOST closed connection,error=%d\n",errno);
         close(aprs_sock);
         aprs_sock = -1;
      }
      else /* if it is WOULDBLOCK, we will not go into a loop here */
         traceit("aprs_process_text: send error=%d\n", errno);
   }
   
   time(&aprs_streamID[rptr_idx].last_time);

   return;
}

static bool aprs_write_data(short int rptr_idx, unsigned char *data)
{

   if ((rptr_idx < 0) || (rptr_idx > 2))
   {
      traceit("ERROR in aprs_write_data: rptr_idx %d is invalid\n", rptr_idx);
      return false;
   }

   if (aprs_pack[rptr_idx].is_sent)
      return false;

   switch (aprs_pack[rptr_idx].sl)
   {
      case sl_first:
         aprs_pack[rptr_idx].buf[0] = data[0] ^ 0x70;
         aprs_pack[rptr_idx].buf[1] = data[1] ^ 0x4f;
         aprs_pack[rptr_idx].buf[2] = data[2] ^ 0x93;
         aprs_pack[rptr_idx].sl = sl_second;
         return false;

      case sl_second:
         aprs_pack[rptr_idx].buf[3] = data[0] ^ 0x70;
         aprs_pack[rptr_idx].buf[4] = data[1] ^ 0x4f;
         aprs_pack[rptr_idx].buf[5] = data[2] ^ 0x93;
         aprs_pack[rptr_idx].sl = sl_first;
         break;
   }

   if ((aprs_pack[rptr_idx].buf[0] & 0xf0) != 0x30)
      return false;

   return aprs_add_data(rptr_idx, aprs_pack[rptr_idx].buf + 1);

}

static void aprs_reset(short int rptr_idx)
{
   if ((rptr_idx < 0) || (rptr_idx > 2))
   {
      traceit("ERROR in aprs_reset: rptr_idx %d is invalid\n", rptr_idx);
      return;
   }

   aprs_pack[rptr_idx].al = al_none;
   aprs_pack[rptr_idx].len = 0;
   aprs_pack[rptr_idx].sl = sl_first;
   aprs_pack[rptr_idx].is_sent = false;

   return;
}

static void aprs_sync_it(short int rptr_idx)
{
   if ((rptr_idx < 0) || (rptr_idx > 2))
   {
      traceit("ERROR in aprs_sync_it: rptr_idx %d is invalid\n", rptr_idx);
      return;
   }

   aprs_pack[rptr_idx].sl = sl_first;
   return;
}

static bool aprs_add_data(short int rptr_idx, unsigned char *data)
{
   unsigned int i;
   unsigned char c;
   bool ok;

   if ((rptr_idx < 0) || (rptr_idx > 2))
   {
      traceit("ERROR in aprs_add_data: rptr_idx %d is invalid\n", rptr_idx);
      return false;
   }

   for (i = 0; i < 5; i++)
   {
      c = data[i];

      if ((aprs_pack[rptr_idx].al == al_none) && (c == '$'))
      {
         aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
         aprs_pack[rptr_idx].len++;
         aprs_pack[rptr_idx].al = al_$1;
      }
      else
      if ((aprs_pack[rptr_idx].al == al_$1) && (c == '$'))
      {
         aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
         aprs_pack[rptr_idx].len++;
         aprs_pack[rptr_idx].al = al_$2;
      }
      else
      if ((aprs_pack[rptr_idx].al == al_$2) && (c == 'C'))
      {
         aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
         aprs_pack[rptr_idx].len++;
         aprs_pack[rptr_idx].al = al_c1;
      }
      else
      if ((aprs_pack[rptr_idx].al == al_c1) && (c == 'R'))
      {
         aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
         aprs_pack[rptr_idx].len++;
         aprs_pack[rptr_idx].al = al_r1;
      }
      else
      if ((aprs_pack[rptr_idx].al  == al_r1) && (c == 'C'))
      {
         aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
         aprs_pack[rptr_idx].len++;
         aprs_pack[rptr_idx].al = al_c2;
      }
      else
      if (aprs_pack[rptr_idx].al == al_c2)
      {
         aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
         aprs_pack[rptr_idx].len++;
         aprs_pack[rptr_idx].al = al_csum1;
      }
      else
      if (aprs_pack[rptr_idx].al == al_csum1)
      {
         aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
         aprs_pack[rptr_idx].len++;
         aprs_pack[rptr_idx].al = al_csum2;
      }
      else
      if (aprs_pack[rptr_idx].al == al_csum2)
      {
         aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
         aprs_pack[rptr_idx].len++;
         aprs_pack[rptr_idx].al = al_csum3;
      }
      else
      if (aprs_pack[rptr_idx].al == al_csum3)
      {
         aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
         aprs_pack[rptr_idx].len++;
         aprs_pack[rptr_idx].al = al_csum4;
      }
      else
      if ((aprs_pack[rptr_idx].al == al_csum4) && (c == ','))
      {
         aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
         aprs_pack[rptr_idx].len++;
         aprs_pack[rptr_idx].al = al_data;
      }
      else
      if ((aprs_pack[rptr_idx].al == al_data) && (c != '\r'))
      {
         aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
         aprs_pack[rptr_idx].len++;

         if (aprs_pack[rptr_idx].len >= 300)
         {
             traceit("ERROR in aprs_add_data: Expected END of APRS data\n");
             aprs_pack[rptr_idx].len = 0;
             aprs_pack[rptr_idx].al  = al_none;
         }
      }
      else
      if ((aprs_pack[rptr_idx].al == al_data) && (c == '\r'))
      {
         aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
         aprs_pack[rptr_idx].len++;


         ok = aprs_check_data(rptr_idx);
         if (ok)
         {
            aprs_pack[rptr_idx].al = al_end;
            return true;
         }
         else
         {
            traceit("BAD checksum in APRS data\n");
            aprs_pack[rptr_idx].al  = al_none;
            aprs_pack[rptr_idx].len = 0;
         }
      }
      else
      {
         aprs_pack[rptr_idx].al  = al_none;
         aprs_pack[rptr_idx].len = 0;
      }
   }
   return false;
}

static unsigned int aprs_get_data(short int rptr_idx, unsigned char *data, unsigned int len)
{
   unsigned int l;

   if ((rptr_idx < 0) || (rptr_idx > 2))
   {
      traceit("ERROR in aprs_get_data: rptr_idx %d is invalid\n", rptr_idx);
      return 0;
   }

   l = aprs_pack[rptr_idx].len - 10;

   if (l > len)
      l = len;

   memcpy(data, aprs_pack[rptr_idx].data  + 10, l);

   aprs_pack[rptr_idx].al = al_none;
   aprs_pack[rptr_idx].len = 0;
   aprs_pack[rptr_idx].is_sent = true;

   return l;

}

static bool aprs_check_data(short int rptr_idx)
{
   unsigned int my_sum;
   char buf[5];

   if ((rptr_idx < 0) || (rptr_idx > 2))
   {
      traceit("ERROR in aprs_check_data: rptr_idx %d is invalid\n", rptr_idx);
      return false;
   }
   my_sum = aprs_calc_crc(aprs_pack[rptr_idx].data + 10,
                   aprs_pack[rptr_idx].len - 10);

   sprintf(buf, "%04X", my_sum);

   return (memcmp(buf,
           aprs_pack[rptr_idx].data + 5,
           4) == 0);

}

static unsigned int aprs_calc_crc(unsigned char* buf, unsigned int len)
{
   unsigned int my_crc = 0xffff;
   unsigned int i,j;
   unsigned char c;
   bool xor_val;

   if (!buf)
      return 0;

   if (len <= 0)
      return 0;

   for (j = 0; j < len; j++)
   {
      c = buf[j];

      for (i = 0; i < 8; i++)
      {
         xor_val = (((my_crc ^ c) & 0x01) == 0x01);
         my_crc >>= 1;

         if (xor_val)
            my_crc ^= 0x8408;

         c >>= 1;
      }
   }
   return (~my_crc & 0xffff);
}

static void gps_send(short int rptr_idx)
{
   time_t tnow = 0;
   char *p = NULL;
   bool ok = false;
   static char old_mycall[CALL_SIZE + 1] = { "        " };

   if ((rptr_idx < 0) || (rptr_idx > 2))
   {
      traceit("ERROR in gps_send: rptr_idx %d is invalid\n", rptr_idx);
      return;
   }

   if (band_txt[rptr_idx].gprmc[0] == '\0')
   {
      band_txt[rptr_idx].gpid[0] = '\0';
      traceit("missing GPS ID\n");
      return;
   }
   if (band_txt[rptr_idx].gpid[0] == '\0')
   {
      band_txt[rptr_idx].gprmc[0] = '\0';
      traceit("Missing GPSRMC\n");
      return;
   }
 
   if (memcmp(band_txt[rptr_idx].gpid, band_txt[rptr_idx].lh_mycall, CALL_SIZE) != 0)
   {
      traceit("MYCALL [%s] does not match first 8 characters of GPS ID [%.8s]\n",
              band_txt[rptr_idx].lh_mycall, band_txt[rptr_idx].gpid);
      band_txt[rptr_idx].gprmc[0] = '\0';
      band_txt[rptr_idx].gpid[0] = '\0';
      return;
   }

   /* if new station, reset last time */
   if (strcmp(old_mycall, band_txt[rptr_idx].lh_mycall) != 0)
   { 
      strcpy(old_mycall, band_txt[rptr_idx].lh_mycall);
      band_txt[rptr_idx].gps_last_time = 0;
   }

   /* do NOT process often */
   time(&tnow);
   if ((tnow - band_txt[rptr_idx].gps_last_time) < 31)
      return;

   traceit("GPRMC=[%s]\n", band_txt[rptr_idx].gprmc);
   traceit("GPS id=[%s]\n",band_txt[rptr_idx].gpid);

   p = strrchr(band_txt[rptr_idx].gprmc, '*');
   if (!p)
   {
      /* BAD news, something went wrong */
      traceit("Missing asterisk before checksum in GPRMC\n");
      band_txt[rptr_idx].gprmc[0] = '\0';
      band_txt[rptr_idx].gpid[0] = '\0';
      return;
   }
   else
   {
      *p = '\0';
      /* verify csum in GPRMC */
      ok = verify_gps_csum(band_txt[rptr_idx].gprmc + 1, p + 1);
      if (!ok)
      {
         traceit("csum in GPRMC not good\n");
         band_txt[rptr_idx].gprmc[0] = '\0';
         band_txt[rptr_idx].gpid[0] = '\0';
         return;
      }
   }

   p = strrchr(band_txt[rptr_idx].gpid, '*');
   if (!p)
   {
      /* BAD news, something went wrong */
      traceit("Missing asterisk before checksum in GPS id\n");
      band_txt[rptr_idx].gprmc[0] = '\0';
      band_txt[rptr_idx].gpid[0] = '\0';
      return;
   }
   else
   {
      *p = '\0';
      /* verify csum in GPS id */
      ok = verify_gps_csum(band_txt[rptr_idx].gpid,  p + 1);
      if (!ok)
      {
         traceit("csum in GPS id not good\n");
         band_txt[rptr_idx].gprmc[0] = '\0';
         band_txt[rptr_idx].gpid[0] = '\0';
         return;
      }
   }

   /* now convert GPS into APRS and send it */
   build_aprs_from_gps_and_send(rptr_idx);

   band_txt[rptr_idx].is_gps_sent = true;
   time(&(band_txt[rptr_idx].gps_last_time));
   return;
}

static void build_aprs_from_gps_and_send(short int rptr_idx)
{
   char buf[512];
   char *p = NULL;
   const char *delim = ",";
   int rc = 0;

   char *saveptr = NULL;

   /* breakdown of GPRMC */
   char *GPRMC = NULL;
   char *time_utc = NULL;
   char *nav = NULL;
   char *lat_str = NULL;
   char *lat_NS = NULL;
   char *lon_str = NULL;
   char *lon_EW = NULL;
   /*** dont care about the rest */

   strcpy(buf, band_txt[rptr_idx].lh_mycall);
   p = strchr(buf, ' ');
   if (p)
   {
      if (band_txt[rptr_idx].lh_mycall[7] != ' ')
      {
         *p = '-';
         *(p + 1) = band_txt[rptr_idx].lh_mycall[7];
         *(p + 2) = '>';
         *(p + 3) = '\0';
      }
      else
      {
        *p = '>';
        *(p + 1) = '\0';
      }
   }
   else
      strcat(buf, ">");

   strcat(buf, "APDPRS,DSTAR*,qAR,");
   strcat(buf, RPTR_ID.rptr[rptr_idx]);
   strcat(buf, ":!");

   GPRMC = strtok_r(band_txt[rptr_idx].gprmc, delim, &saveptr);
   time_utc = strtok_r(NULL, delim, &saveptr);
   nav = strtok_r(NULL, delim, &saveptr);
   lat_str = strtok_r(NULL, delim, &saveptr);
   lat_NS = strtok_r(NULL, delim, &saveptr);
   lon_str = strtok_r(NULL, delim, &saveptr);
   lon_EW = strtok_r(NULL, delim, &saveptr);

   if (lat_str && lat_NS)
   {
      if ((*lat_NS != 'N') && (*lat_NS != 'S'))
      {
         traceit("Invalid North or South indicator in latitude\n");
         return;
      }
      if (strlen(lat_str) != 9)
      {
         traceit("Invalid latitude\n");
         return;
      }
      if (lat_str[4] != '.')
      {
         traceit("Invalid latitude\n");
         return;
      }
      lat_str[7] = '\0';
      strcat(buf, lat_str);
      strcat(buf, lat_NS);
   }
   else
   {
      traceit("Invalid latitude\n");
      return;
   }
   /* secondary table */
   strcat(buf, "\\");

   if (lon_str && lon_EW)
   {
      if ((*lon_EW != 'E') && (*lon_EW != 'W'))
      {
         traceit("Invalid East or West indicator in longitude\n");
         return;
      }
      if (strlen(lon_str) != 10)
      {
         traceit("Invalid longitude\n");
         return;
      }
      if (lon_str[5] != '.')
      {
         traceit("Invalid longitude\n");
         return;
      }
      lon_str[8] = '\0';
      strcat(buf, lon_str);
      strcat(buf, lon_EW);
   }
   else
   {
      traceit("Invalid longitude\n");
      return;
   }

   /* Just this symbolcode only */
   strcat(buf, "k");
   strncat(buf, band_txt[rptr_idx].gpid + 13, 32);

   // traceit("Built APRS from old GPS mode=[%s]\n", buf);
   strcat(buf, "\r\n");

   rc = writen(buf, strlen(buf));
   if (rc == -1)
   {
      if ((errno == EPIPE) ||
          (errno == ECONNRESET) ||
          (errno == ETIMEDOUT) ||
          (errno == ECONNABORTED) ||
          (errno == ESHUTDOWN) ||
          (errno == EHOSTUNREACH) ||
          (errno == ENETRESET) ||
          (errno == ENETDOWN) ||
          (errno == ENETUNREACH) ||
          (errno == EHOSTDOWN) ||
          (errno == ENOTCONN))
      {
         traceit("build_aprs_from_gps_and_send: APRS_HOST closed connection,error=%d\n",errno);
         close(aprs_sock);
         aprs_sock = -1;
      }
      else
         traceit("build_aprs_from_gps_and_send: send error=%d\n", errno);
   }
   return;
}

static bool verify_gps_csum(char *gps_text, char *csum_text)
{
   short int len;
   short int i;
   char c;
   short computed_csum = 0;
   char computed_csum_text[16];
   char *p = NULL;

   len = strlen(gps_text);
   for (i = 0; i < len; i++)
   {
      c = gps_text[i];
      if (computed_csum == 0)
         computed_csum = (char)c;
      else
         computed_csum = computed_csum ^ ((char)c);
   }
   sprintf(computed_csum_text, "%02X", computed_csum);
   // traceit("computed_csum_text=[%s]\n", computed_csum_text);

   p = strchr(csum_text, ' ');
   if (p)
      *p = '\0';

   if (strcmp(computed_csum_text, csum_text) == 0)
      return true;
   else
      return false;
}

static ssize_t writen(char *buffer, size_t n)
{
   ssize_t num_written = 0;
   size_t tot_written = 0;
   char *buf;

   buf = buffer;
   for (tot_written = 0; tot_written < n;)
   {
      num_written = write(aprs_sock, buf, n - tot_written);
      if (num_written <= 0)
      {
         if ((num_written == -1) && (errno == EINTR))
            continue;
         else
            return num_written;
      }
      tot_written += num_written;
      buf += num_written;
   }
   return tot_written;
}


