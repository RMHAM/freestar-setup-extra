
/* by KI4LKF */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <argp.h>
#include <iostream>
#include <fstream>
#include <libgen.h>
#include <sys/sysinfo.h>
#include <ctemplate/template.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <sys/stat.h>

// Messages (to/from g2_link.cpp; 255 on receive matches anything
struct message
{
   size_t count;
   unsigned char query[28];
};

// Function prototypes (in order of appearance after main()
static void verbose(int, const char [], ...);
static error_t cmdLine(int, char **);
static error_t parse_opt(int, char *, struct argp_state *);
static bool srv_open(in_addr *);
static void srv_close();
static void sigCatch(int);
static void g2Request(const message *);
static void g2Login(char *, char *);
static size_t g2Receive();
bool g2MsgCmp(const message *, size_t);
void adduser(ctemplate::TemplateDictionary*, unsigned char *);
void addheard(unsigned char *, char *);
static bool islocal(in_addr *);
static void xrflinked(unsigned char *, size_t);

const short keepalive = 3;		// Normally 3 seconds wait for reply
const short maxexec = 5;		// Normally 5 seconds to get all replies

using namespace std;


#define VERSION "v2.02"

//v2.02	 - Chris, VE3NRT:  Add stat.h to includes for chmod in some linuxes
//v2.01  - Chris, VE3NRT:  Fix duplication of software client section
//			   Fix segfault if no directory in outfile spec
//			   Includes some preliminary code for reflector support
//v2.00  - Chris, VE3NRT:  Restructure code, use ctemplate, add command line options
//v1.14e - Ramesh, VA3UV:  Added version # in sub-header
//v1.14d - Ramesh, VA3UV:  Added uptime stats

static ctemplate::TemplateDictionary* dict;
std::fstream of;
std::string output;
static char g2_link_version[]="----";

const struct message qConnect = {5, {5, 0, 24, 0, 1}};		// Connect message (R/W)
const struct message qDisconnect = {5, {5, 0, 24, 0, 0}};	// Disconnect
const struct message qKeepAlive = {3, {3, 96, 0}};		// Handshake while waiting (R/W)
const struct message qLoginW = {4, {28, 192, 4, 0}};		// Log in to g2_link (2 addt'l params) 
const struct message qLoginR = {8, {8, 192, 4, 0, 79, 75, 82, 255}}; // Log in OK response
const struct message qLoginFailed = {8, {8, 192, 4, 0, 255, 255, 255, 255}}; // Log in failed response
const struct message qVersionW = {4, {4, 0xC0, 3, 0}};		// Request for version information
const struct message qLinkedNodesW = {4, {4, 0xC0, 5, 0}};	// Request for linked nodes
const struct message qUsersW = {4, {4, 0xC0, 6, 0}};		// Request for connected users
const struct message qLastHeardW = {4, {4, 0xC0, 7, 0}};	// Request for last heard remote users
const struct message qVersionR = {9, {255, 255, 3, 0, 255, 255, 255, 255, 255}}; // Version # response
const struct message qLinkedNodesR = {9, {255, 255, 5, 1, 255, 255, 255, 255, 255}}; // Linked nodes response
const struct message qUsersR = {9, {255, 255, 6, 0, 255, 255, 255, 255, 255}}; // Connected users response
const struct message qLastHeardR = {10, {255, 255, 7, 0, 255, 255, 255, 255, 255, 255}}; // Last heard response

static bool keep_running = true;
static int g2_sock = -1;
static unsigned char queryCommand[2048];
static char temp_string[80];
static struct sockaddr_in toLink;
static struct sockaddr_in fromLink;

// Command line parameters

static const int numargs = 1;
static char rptrcall[7];			// The only command line parameter - required
static char modules[6] = "ABCDE";
static char regptr[80] = "";
static int verbosity = 1;
static char banner[32] = "Free*Star";
static char password[] = "DV019999";		// Appears to have no effect.
static char lhuser[7] = "1NFO";
static in_addr ip = {0x0200000A};		// 10.0.0.2 - this might have problems on big-endian systems
static unsigned int port = 20001;		// Standard port
static bool debug = 0;
static bool use_outfile = 0;			// When 0, use stdout without a template file
static bool use_tempfile = 0;			// Use a temporary file then rename it to outfile
static char outfile[256] = "index.html";
static char tempfile[256];			// Pointer to block allocate by tempnam function
static char templatefile[256] = "g2_lh.tpl";
static long p;					// Bitmask of modules to be reported
static bool reflector = 0;			// Reflector vs. Repeater mode

int main(int argc, char **argv)
{
   static size_t i;
   static size_t j;
   fd_set fdset;
   int recvlen;
   time_t init_rq;
   time_t tnow;
   tm btnow;					// Broken down time
   short total_keepalive = keepalive;
   struct sigaction act;
   bool login_ok = false;
   static char linked[5][11] = {"Not linked", "Not linked", "Not linked", "Not linked", "Not linked"};
   struct sysinfo sys_info;

   verbose(3, "g2_lh dashboard Version %s\n", VERSION);
   cmdLine(argc, argv);

   act.sa_handler = sigCatch;
   sigemptyset(&act.sa_mask);
   act.sa_flags = SA_RESTART;

   if (sigaction(SIGTERM, &act, 0) != 0)
   {
      verbose(1, "sigaction-TERM failed, error=%d\n", errno);
      return 1;
   }
   if (sigaction(SIGINT, &act, 0) != 0)
   {
      verbose(1, "sigaction-INT failed, error=%d\n", errno);
      return 1;
   }

   if (!srv_open(&ip))
   {
      verbose(3, "srv_open() failed\n");
      return 1;
   }

   /* initiate login */
   verbose(3, "Requesting connection...\n");
   g2Request(&qConnect);
   time(&init_rq);

   /* Initialize ctemplate system */
   dict = new ctemplate::TemplateDictionary("dashboard");
   dict->SetValue("TITLE", banner);
   dict->SetValue("BANNER", banner);
   dict->SetValue("CALL", rptrcall);
   dict->SetValue("LHVER", VERSION);

   while (keep_running)
   {
      if (recvlen = g2Receive())
      {
         if (g2MsgCmp(&qKeepAlive, recvlen))
         {
            g2Request(&qKeepAlive);
            if (--total_keepalive == 0)
            {
               verbose(3, "Waited %i seconds for dashboard info.\n",keepalive);
               break;
            }
         }
         else if (g2MsgCmp(&qConnect, recvlen))
         {
            verbose(3, "Connected...\n");
            g2Login(lhuser, password);
         }
         else if (g2MsgCmp(&qLoginR, recvlen))
         {
            verbose(3, "Login OK, requesting gateway info...\n");
            login_ok = true;

            g2Request(&qVersionW);			// Request version
            g2Request(&qLinkedNodesW);			// Request linked nodes
            g2Request(&qUsersW);			// Request connected users
            g2Request(&qLastHeardW);			// Request last heard
         }
         else if (g2MsgCmp(&qLoginFailed, recvlen))	// First 4 bytes of previous message + anything
         {
            verbose(1, "Login failed\n");
            break;
         }
         else if (g2MsgCmp(&qVersionR, recvlen))
         {
            memcpy(g2_link_version, queryCommand + 4, 4);
            g2_link_version[4] = '\0';
            dict->SetValue("G2VER", g2_link_version);
         }
         else if (g2MsgCmp(&qLinkedNodesR, recvlen))
         {
            verbose(3, "Received Linked Nodes Message (%i bytes)\n", recvlen);	// v2.01
            if (reflector)							// v2.01
            {
               verbose(2, "Strangely in reflector code for linked nodes value %i\n", reflector);
               xrflinked(queryCommand, recvlen);
            }
            else
               for (i=8; i<recvlen; i+=20)
                  if ('A'<=queryCommand[i] && queryCommand[i] <= 'E')
                  {
                     j = queryCommand[i]-'A';
                     memcpy(linked[j],&queryCommand[i+1],8);
                     linked[j][8] = 0;
                     verbose(4, "Module %c is linked\n", queryCommand[i]);
                  }
         }
         else if (g2MsgCmp(&qUsersR, recvlen))
         {
            ctemplate::TemplateDictionary* users = dict->AddSectionDictionary("USERS");	// v2.01
            for (i=8; i<recvlen; i+=20)							// v2.01
               adduser(users, &queryCommand[i]);					// v2.01
         }
         else if (g2MsgCmp(&qLastHeardR, recvlen))
         {
            for (i=10; i<recvlen; i+=24)
               addheard(&queryCommand[i],rptrcall);
         }
         FD_CLR (g2_sock,&fdset);
      }
      time(&tnow);
      if ((tnow - init_rq) > maxexec)
      {
         verbose(1, "timeout... no reply from g2_link ...\n");
         verbose(3, "possible reasons: incorrect g2_link IP address, or g2_link not running, or duplicate client\n");
         keep_running = false;
      }
   }

   if (login_ok)
   {
      verbose(3, "Disconnecting\n");
      g2Request(&qDisconnect);

// Registration pointer

      if (regptr[0])
      {
         ctemplate::TemplateDictionary* reg = dict->AddSectionDictionary("REG");
         reg->SetValue("REGURL", regptr);
      }

// Linked nodes
      if (modules[0])
      {
         ctemplate::TemplateDictionary* linkednodes = dict->AddSectionDictionary("LINKED");
         for (i=0;modules[i]!=0;i++)
         {
            ctemplate::TemplateDictionary* node = dict->AddSectionDictionary("NODE");
            node->SetFormattedValue("MODULE", "%c", modules[i]);
            node->SetValue("CALL",linked[modules[i]-'A']);
         }
      }

// Date & Time
      localtime_r(&tnow, &btnow);
      strftime(temp_string, sizeof(temp_string), "%A, %F %H:%M:%S %Z", &btnow);
      dict->SetValue("NOW", temp_string);
 
// Gateway uptime - note that this is the uptime of the system running g2_lh, not g2_link

      if(islocal(&ip) && sysinfo(&sys_info) == 0)
      {
         ctemplate::TemplateDictionary* uptime = dict->AddSectionDictionary("UPTIME");
         uptime->SetIntValue("DAY", sys_info.uptime / 86400);
         uptime->SetValue("DAYS", sys_info.uptime>=172800 ? "s" : "");
         uptime->SetIntValue("HOUR", (sys_info.uptime % 86400) / 3600);
         uptime->SetValue("HOURS", (sys_info.uptime % 86400) >= 7200 ? "s" : "");
         uptime->SetIntValue("MINUTE", (sys_info.uptime % 3600) / 60);
         uptime->SetValue("MINUTES", (sys_info.uptime % 3600) >= 120 ? "s" : "");
      }

// ctemplate expansion goes here
      if (debug)
         ctemplate::ExpandTemplate(templatefile, ctemplate::STRIP_BLANK_LINES, dict, &output);
      else
         ctemplate::ExpandTemplate(templatefile, ctemplate::STRIP_WHITESPACE, dict, &output);
      if (use_outfile)
         if (use_tempfile)
         {
            if (mkstemp(tempfile) == -1)
            {
               verbose(2, "Can't create temporary file %s, writing output file directly\n", tempfile);
               ofstream of(outfile);
               of << output;
               of.close();
               if(chmod(outfile, 0644))
                  verbose(1,"Can't change mode for file %s\n", outfile);
            }
            else
            {
               ofstream of(tempfile);
               of << output;
               of.close();
               if (!chmod(tempfile, 0644))
               {
                  if (rename(tempfile, outfile) == -1)
                     verbose(1, "Can't rename temporary file %s to %s\n", tempfile, outfile);
               }
               else
                  verbose(1,"Can't change mode for temporary file %s\n", tempfile);
            }
         }
         else
         {
            ofstream of;
            of.open(outfile);
            of << output;
            of.close();
            if (chmod(outfile, 0644))
               verbose(1,"Can't change mode for file %s\n", outfile);
         }
      else
         std::cout << output;
      return 0;

   }
}; // main

// Function prints message if it exceeds specified severity level
static void verbose(int lvl, const char *fmt, ...)
{ 
    va_list vars;
    if (lvl <= verbosity)
    {
       va_start(vars, fmt);
       vfprintf(stderr, fmt, vars);
       va_end(vars);
    }
    return;
}; // verbose

static error_t cmdLine(int argc, char** argv)
{
   const char *argp_program_version = "v2.00";
   const char *argp_program_bug_address = "";			// Set to email where to report bugs
   const char argp_args_doc[] = "g2_lh [options] <repeater call sign>";
   const char doc[] = "Report connection status\v"
		      "The g2_lh dashboard requests and reports the connection status of \n"
		      "a D-STAR or FREE*STAR repeater's modules, software clients, local \n"
		      "RF users and remote users from other repeaters or reflectors. \n"
		      "It creates an html file which can be made available to the system's \n"
		      "web server.";

   static struct argp_option options[] = {
      {"address", 'a', "10.0.0.2", 0, "IP Address for g2_link"},
      {"banner", 'b', "Free*Star", 0,"Banner message"},
      {"debug", 'd', 0, OPTION_ARG_OPTIONAL, "Make output file human readable"},
      {"direct", 'O', "file", 0, "Direct output html file spec"},
      {"modules", 'm', "ABCDE", 0, "Report link status on modules"},
      {"output", 'o', "stdout", 0, "Output html file spec (renamed from temporary file)"},
      {"password", 'P', "string", OPTION_HIDDEN, "G2 Link password"},
      {"port", 'p', "20001", 0, "IP Port number for g2_link"},
      {"register", 'r', "DStar.do", OPTION_ARG_OPTIONAL, "Registration pointer"},
      {"template", 't', "file", 0, "file specification for template"},
      {"user", 'u', "call", 0, "Dashboard user's call sign"},
      {"verbose", 'v', "level", OPTION_ARG_OPTIONAL, "output to stderr: 0=silent, 1=errors, 2=warning, 3=Information"}, 
      {0}
   };

   static struct argp args = {options, parse_opt, "g2_lh [options] <repeater call sign>"};

   return argp_parse(&args, argc, argv, 0, 0, 0);

}; // cmdLine
   
static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
static int i;
static char *ptr;
static char c;

   switch (key)
   {
      case ARGP_KEY_ARG:
         if (state->arg_num < numargs)
         {
            if (strlen(arg) < sizeof(rptrcall))
               if (strlen(arg) <= 2)
                  argp_error(state, "Call sign is too short");
               else
               {
                  for (i=0;arg[i]!=0;i++)
                  {
                     rptrcall[i] = toupper(arg[i]);
                     if (!isalnum(rptrcall[i]))
                        argp_error(state, "Invalid characters in call sign");
                  }
               }
            else
               argp_error(state, "Too many characters in call sign");
         }
         else
            argp_error(state, "Too many arguments");
         break;
      case ARGP_KEY_NO_ARGS:
         argp_error(state, "Repeater call sign required");
         break;
      case ARGP_KEY_SUCCESS:
         if (!(reflector = !strncmp("XRF", rptrcall, 3)) && (p & ~0x1f))
            argp_error(state, "Invalid module(s) specified");
      case 'a':						// Address of repeater - default 10.0.0.2
         if (arg)
            if (!inet_aton(arg, &ip))
               argp_error(state, "Invalid g2_link IP address");
         break;
      case 'b':
         if (arg)
            if (strlen(arg) < sizeof(banner))
               strcpy(banner, arg);
            else
               argp_error(state, "Banner message exceeds 32 characters");
         break;
      case 'd':
         debug = 1;
         break;
      case 'm':
         if (arg)
         {
            p = 0;
            for (i=0;arg[i]!=0;i++)
            {
               c = toupper(arg[i]) - 'A';
               if (0 <= c && c <= 26)
                  p = p | (1 << c);
               else
                  argp_error(state, "Invalid module name");
            }
            for (i=0, ptr=modules;i<5;i++)
               if (p & (1 << i))
                  *ptr++ = 'A' + i;
            *ptr = 0;
         }
         break;
      case 'o':
         parse_opt('O', arg, state);
         if (use_outfile)
         {
            if (strlen(outfile) < sizeof(tempfile))
            {
               ptr = strcpy(tempfile,outfile);
               if (strlen(dirname(tempfile))!=1)		// v2.01
                 ptr += strlen(dirname(tempfile));
               if (strlen(tempfile) + strlen("/g2_lhXXXXXX") < sizeof(tempfile))
               {
                  strcpy(ptr, "/g2_lhXXXXXX");
                  use_tempfile=1;
               }
            }
            else
               argp_error(state, "Output (temp) file specification exceeds maximum length");
            return 0;
         }
         break;							// v2.01
      case 'O':
         if (arg)
         {
            if (strlen(arg) < sizeof(outfile))
               if (strlen(arg) == 1 || arg[0] == '-')
                  use_outfile = 0;
               else
               {
                  strcpy(outfile, arg);
                  use_outfile = 1;
               }
            else
               argp_error(state, "Output file specification exceeds maximum length");
         }
         break;
      case 'p':
         if (arg)
         {
            port = strtol(arg, &ptr, 0);
            if (!*ptr)
               argp_error(state, "Invalid port number");
         }
         break;
      case 'P':
         if (arg)
         {
            if (strlen(arg) < sizeof(password))
               strcpy(password, arg);
            else
               argp_error(state, "Password is too long");
         }
         break;
      case 'r':						// registration pointer, default Dstar.do
         if (arg)					// If specified without arg, don't show one
         {
            if (strlen(arg) < sizeof(regptr))
               strcpy(regptr, arg);
            else
               argp_error(state, "Registration URL is too long");
         }
         else
         {
            strcpy(regptr, "Dstar.do");
            if ((numargs + state->next) < state->argc)
            {
               if (strlen(state->argv[state->next]) < sizeof(regptr))
               {
                  for (ptr=state->argv[state->next]; *ptr; ptr++)
                     if (!(isalnum(*ptr) || *ptr == '/' || *ptr == ':' || *ptr == '.'))
                        break;
                  if (!*ptr)
                     parse_opt(key, state->argv[(state->next)++], state);
               }      
            }
         }
         break;
      case 't':
         if (arg)
         {
            if (strlen(templatefile) < sizeof(templatefile))
               strcpy(templatefile, arg);
            else
               argp_error(state, "Template file specification exceeds maximum length");
         }
         break;
      case 'u':
         if (state->arg_num == 0)
         {
            if (strlen(arg) < sizeof(rptrcall))
               if (strlen(arg) <= 2)
                  argp_error(state, "User call sign is too short");
               else
               {
                  for (i=0;arg[i]!=0;i++)
                  {
                     lhuser[i] = islower(arg[i]) ? toupper(arg[i]) : arg[i];
                     if (!isalnum(lhuser[i]))
                        argp_error(state, "Invalid characters in user call sign");
                  }
               }
            else
               argp_error(state, "Too many characters in user call sign");
         }
         else
            argp_error(state, "Too many arguments");
         break;
      case 'v':						// verbosity, default arg is 3
         if (arg)
         {
            verbosity = strtol(arg, &ptr , 0);
            if (*ptr)
               argp_error(state, "Invalid verbosity level (use 0-3)");
         }
         else
         {
            verbosity = 3;
            if ((numargs + state->next) < state->argc)
            {
               strtol(state->argv[state->next], &ptr, 0);
               if (!*ptr)
               {
                  parse_opt(key, state->argv[(state->next)++], state);
               }
            }
         }
         break;
      default:
         return ARGP_ERR_UNKNOWN;
   }
   return 0;
}; // parse_opt

/* signal catching function */
static void sigCatch(int signum)
{
   /* do NOT do any serious work here */
   if ((signum == SIGTERM) || (signum == SIGINT))
      keep_running = false;
   return;
}; // sigCatch
      
static bool srv_open(in_addr *ip)
{
   /* create our gateway socket */ 
   g2_sock = socket(PF_INET,SOCK_DGRAM,0);
   if (g2_sock == -1)
   {
      verbose(1, "Failed to create gateway socket,errno=%d\n",errno);
      return false;
   }

   memset(&toLink,0,sizeof(struct sockaddr_in));
   toLink.sin_family = AF_INET;
   toLink.sin_addr.s_addr = ip->s_addr;
   toLink.sin_port = htons(port);

   fcntl(g2_sock,F_SETFL,O_NONBLOCK);
   return true;
}; // srv_open

static void srv_close()
{
   if (g2_sock != -1)
      close(g2_sock);
   return;
};  // srv_close

// Issue a request to the g2_link program
static void g2Request(const message *msg)
{
   sendto(g2_sock, &msg->query, msg->count, 0,
          (struct sockaddr *)&toLink, sizeof(struct sockaddr_in));
   return;
}; //g2Request

// Login request to g2_link
static void g2Login(char *lhuser, char *password)
{
   const size_t maxcall = 8;		// Maximum # of characters in a call sign
   static struct message logreq;
   static int i;
   assert(sizeof(logreq.query)>=28);
   for (logreq.count=0;logreq.count<qLoginW.count;logreq.count++)
        logreq.query[logreq.count] = qLoginW.query[logreq.count];
   for (i=0;lhuser[i]!=0;i++,logreq.count++) logreq.query[logreq.count] = lhuser[i];
   for (;logreq.count<20;logreq.count++) logreq.query[logreq.count] = 0;
   for (i=0;i<strlen(password) && logreq.count < 28;i++,logreq.count++)
        logreq.query[logreq.count] = password[i];
   for (;logreq.count<28;logreq.count++) logreq.query[logreq.count] = 0;
   g2Request(&logreq);
   return;
}; // g2Login

// Check if supplied IP address is on this system

static size_t g2Receive()
{
   socklen_t fromlen;
   size_t recvlen;
   fd_set fdset;
   FD_ZERO(&fdset);
   FD_SET(g2_sock, &fdset);

   struct timeval tv;
   tv.tv_sec = 1;
   tv.tv_usec = 0;

   while (1) 
   {
      if (!FD_ISSET(g2_sock, &fdset))
         return 0;
      (void)select(g2_sock + 1, &fdset, 0, 0, &tv);
      fromlen = sizeof(struct sockaddr_in);
      recvlen = recvfrom(g2_sock, (char *)queryCommand, sizeof(queryCommand), 0,
                         (struct sockaddr *)&fromLink, &fromlen);
      if (fromLink.sin_addr.s_addr == toLink.sin_addr.s_addr)
         return recvlen;
   }
}; //g2Receive

bool g2MsgCmp(const message *msg, size_t rcvlen)
{
   static int i;
   if (msg->count > rcvlen) return false;
   for (i=0;i<msg->count;i++)
      if (msg->query[i] != 255 && msg->query[i] != queryCommand[i])
         return false;
   return true;
}; //g2MsgCmp

void adduser(ctemplate::TemplateDictionary* users, unsigned char *item)
{
   static char call[9];
   static char m[2];				// For module identifier
   static int i;

   if (memcmp(lhuser,(char *)&item[1],strlen(lhuser)))	// v2.01
   {
      ctemplate::TemplateDictionary* user = users->AddSectionDictionary("USER");
// What call sign is connected?
      memcpy(call,&item[1],8);
      call[8]=0;
      for (i=7;call[i] == ' ';)
         call[i--] = 0;
      user->SetValue("CALL",call);

// What module are we connected to?
      if (item[0] == ' ')
         user->SetValue("MODULE","Listening");
      else
         user->SetFormattedValue("MODULE", "%c", item[0]);

// What type of device is connected?
      switch (item[9])
      {
         case 'H':
            user->SetValue("TYPE","Hotspot");
            break;
         case 'D':
            user->SetValue("TYPE","DVAP Dongle");
            break;
         case 'X':
         default:
            user->SetValue("TYPE","DV Dongle");
      }
   }
   return;
}; //adduser

void addheard(unsigned char *item, char *gw)
{
   static int i;
   static time_t ts;				// timestamp
   static struct tm bts;			// Broken time representation
   static char section[7];

   if (item[14] == 'l' || item[14] == 'g')
      strcpy(section,"LOCAL");
   else
      strcpy(section,"REMOTE");
   ctemplate::TemplateDictionary* heard = dict->AddSectionDictionary(section);

// Call sign heard
   memcpy(temp_string, item, 8);
   temp_string[8]=0;
   for (i=7;temp_string[i] == ' ';)
      temp_string[i--] = 0;
   heard->SetValue("CALL", temp_string);
   for (i=0;i<strlen(temp_string);i++)
      if (temp_string[i] == ' ') temp_string[i] = '-';
   heard->SetValue("APRSCALL", temp_string);

// Time
   localtime_r((time_t *) &item[16], &bts);
   strftime(temp_string, sizeof(temp_string), "%F %H:%M:%S %Z", &bts);
   heard->SetValue("TIMESTAMP", temp_string);

// TX Repeater [and remote source]

   memcpy(temp_string, &item[8], 6);
   temp_string[6] = ' ';
   if (item[14] == 'l' || item[14] == 'g')
   {
      temp_string[7] = item[15];
      temp_string[8] = 0;
      heard->SetValue("TX", temp_string);
      if (item[14] == 'g')
         ctemplate::TemplateDictionary* tx = heard->AddSectionDictionary("GPS");
      else
         ctemplate::TemplateDictionary* tx = heard->AddSectionDictionary("NOGPS");
   }
   else
   {
      temp_string[7] = item[14];
      temp_string[8] = 0;
      heard->SetValue("SOURCE", temp_string);
      for (i=0;gw[i]!=0;i++)
         temp_string[i] = gw[i];
      temp_string[i++] = ' ';
      temp_string[i++] = item[15];
      temp_string[i] = 0;
      heard->SetValue("TX", temp_string);
   }
   return;
}; //addheard

static void xrflinked(unsigned char *buf, size_t recvlen)
{
   unsigned char *mod[26];			// Up to 26 modules
   unsigned int i;
   bool run = true;
   
   ctemplate::TemplateDictionary* xrf = dict->AddSectionDictionary("XRF");
   for (i=0;i<26;mod[i++]=buf);			// Initialize module pointers
   for (i=0;modules[i]!=0;i++)
      xrf->SetFormattedValue("MODULE","%c",modules[i]);
   while (run)
   {
      run = false;
      for (i=0;modules[i]!=0;i++)		// Scan each module
         for (;mod[modules[i]-'A']<buf+recvlen;mod[modules[i]-'A']+=20)
            if (*mod[modules[i]-'A'] == modules[i])
            {
               run = true;
               break;
            }
      if (run)
      {
         ctemplate::TemplateDictionary* xrfl = xrf->AddSectionDictionary("XRFL");
         for (i=0;modules[i]!=0;i++)
         {
            if (mod[modules[i]-'A'] < buf+recvlen)
            {   
               memcpy (temp_string, mod[modules[i]-'A']+1, 8);
               temp_string[8]=0;
               xrfl->SetValue("CALL", temp_string);
            }
            else 
               xrfl->SetValue("CALL", "");
         }
      }
   }
}

static bool islocal(in_addr *ip)
{
   struct ifaddrs *ifaddr, *ifa;

   if (getifaddrs(&ifaddr) == -1)
   {
      verbose(2, "Call to getifaddrs failed - assuming g2_link is local\n");
      return true;
   }
   else
   {
      for (ifa = ifaddr; ifa; ifa = ifa->ifa_next)
         if (ifa->ifa_addr)
            if (ifa->ifa_addr->sa_family == AF_INET)
               if (ip->s_addr == ((sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr)
                   return true;
      return false;
   }
}; // islocal

