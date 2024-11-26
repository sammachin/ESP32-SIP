
static const char __attribute__((unused)) * TAG = "SIP";

#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sip.h"
#include "siptools.h"

#define	SIP_PORT	5060
#define	SIP_RTP		8888
#define	SIP_MAX		1500

static TaskHandle_t
make_task (const char *tag, TaskFunction_t t, const void *param, int kstack)
{                               // Make a task
   if (!kstack)
      kstack = 8;               // Default 8k
   TaskHandle_t task_id = NULL;
   xTaskCreate (t, tag, kstack * 1024, (void *) param, 2, &task_id);
   if (!task_id)
      ESP_LOGE (TAG, "Task %s failed", tag);
   return task_id;
}

static uint32_t
uptime (void)
{
   return esp_timer_get_time () / 1000000LL ? : 1;
}

int
replacestring (char **target, const char *new)
{                               // Replace a string pointer with new value, malloced, returns non zero if changed - empty strings are NULL
   if (new && !*new)
      new = NULL;
   char *old = *target;
   if (new && old && !strcmp (old, new))
      return 0;                 // No change
   if (!new && !old)
      return 0;                 // No change
   if (new)
      *target = strdup (new);
   else
      *target = NULL;
   free (old);
   return 1;
}

static void sip_task (void *arg);
static void sip_audio_task (void *arg);

typedef enum __attribute__((__packed__))
{
   TASK_IDLE,                   // Not in a call
      TASK_OG_INVITE,           // We are sending INVITEs awaiting any response
      TASK_OG_WAIT,             // We have 1XX and waiting, we will send CANCELs if hangup set
      TASK_OG,                  // We are in an outgoing call
      TASK_OG_BYE,              // We are sending BYEs, awaiting reply
      TASK_IC_ALERT,            // We are sending 180
      TASK_IC_BUSY,             // We are sending 486, waiting ACK
      TASK_IC_OK,               // We are sending 200, waiting ACK
      TASK_IC,                  // We are in an incoming call
      TASK_IC_BYE,              // We are sendin BYEs, awaiting reply
} sip_task_state_t;

static struct
{                               // Local data
   TaskHandle_t task;           // Task handle
   SemaphoreHandle_t mutex;     // Mutex for this structure
   sip_callback_t *callback;    // The registered callback functions
   char *callid;                // Current call ID - we handle only one call at a time
   char *ichost;                // Registration details
   char *icuser;                // Registration details
   char *icpass;                // Registration details
   char *ogcli;                 // Outgoing call details
   char *oghost;                // Outgoing call details
   char *oguri;                 // Outgoing call details
   char *oguser;                // Outgoing call details
   char *ogpass;                // Outgoing call details
   uint32_t regexpiry;          // Registration expiry
   sip_state_t state;           // Status reported by sip_callback
   uint8_t call:1;              // Outgoing call required
   uint8_t answer:1;            // Answer required
   uint8_t hangup:1;            // Hangup required
} sip = { 0 };

static int
gethost (const char *name, uint16_t port, struct sockaddr_storage *addr)
{
   int len = 0;
   const struct addrinfo hint = {
      .ai_family = AF_UNSPEC,
      .ai_socktype = SOCK_DGRAM,
      .ai_flags = AI_NUMERICSERV,
   };
   struct addrinfo *res = NULL;
   memset (addr, 0, sizeof (*addr));
   char ports[6];
   sprintf (ports, "%d", port);
   if (!getaddrinfo (name, ports, &hint, &res) && res->ai_addrlen)
   {
      memcpy (addr, res->ai_addr, res->ai_addrlen);
      len = res->ai_addrlen;
   }
   freeaddrinfo (res);
   return len;
}

static uint32_t
sip_request (void *buf, struct sockaddr_storage *addr, socklen_t addrlen, const char *method, const char *uri)
{                               // make a SIP request
   if (!method || !uri || strlen (uri) > SIP_MAX - 100)
      return 0;
   // TODO this needs expanding
   char *p = buf,
      *e = p + SIP_MAX;
   p += sprintf (p, "%s sip:%s SIP/2.0\r\n", method, uri);



   return p - (char *) buf;
}

// Start sip_task, set up details for registration (can be null if no registration needed)
void
sip_register (const char *host, const char *user, const char *pass, sip_callback_t * callback)
{
   sip.callback = callback;
   if (!sip.task)
   {
      sip.mutex = xSemaphoreCreateBinary ();
      xSemaphoreGive (sip.mutex);
      sip.task = make_task ("sip", sip_task, NULL, 8);
   }
   xSemaphoreTake (sip.mutex, portMAX_DELAY);
   if (replacestring (&sip.ichost, host) + replacestring (&sip.icuser, user) + replacestring (&sip.icpass, pass))
      sip.regexpiry = 0;        // Register
   xSemaphoreGive (sip.mutex);
}

// Set up an outgoing call, proxy optional (taken from uri)
int
sip_call (const char *cli, const char *uri, const char *proxy, const char *user, const char *pass)
{
   xSemaphoreTake (sip.mutex, portMAX_DELAY);
   if (sip.state <= SIP_REGISTERED)
   {
      replacestring (&sip.ogcli, cli);
      replacestring (&sip.oghost, proxy);
      replacestring (&sip.oguri, uri);
      replacestring (&sip.oguser, user);
      replacestring (&sip.ogpass, pass);
      sip.call = 1;
   }
   xSemaphoreGive (sip.mutex);
   return 0;
}

// Answer a call
int
sip_answer (void)
{
   xSemaphoreTake (sip.mutex, portMAX_DELAY);
   if (sip.state == SIP_IC_ALERT)
      sip.answer = 1;
   xSemaphoreGive (sip.mutex);
   return 0;
}

// Hangup, cancel, or reject a call
int
sip_hangup (void)
{
   xSemaphoreTake (sip.mutex, portMAX_DELAY);
   if (sip.state > SIP_REGISTERED)
      sip.hangup = 1;
   xSemaphoreGive (sip.mutex);
   return 0;
}

static void
sip_task (void *arg)
{
   // Set up sockets
   int sock = socket (AF_INET, SOCK_DGRAM, IPPROTO_IP);
   if (sock < 0)
   {
      ESP_LOGE (TAG, "SIP Socket failed");
      vTaskDelete (NULL);
      return;
   }
   struct sockaddr_in dest_addr = {.sin_addr.s_addr = htonl (INADDR_ANY),.sin_family = AF_INET,.sin_port = htons (SIP_PORT)
   };
   if (bind (sock, (struct sockaddr *) &dest_addr, sizeof (dest_addr)))
   {
      ESP_LOGE (TAG, "SIP Bind failed");
      vTaskDelete (NULL);
      return;
   }
   make_task ("sip-audio", sip_audio_task, NULL, 8);
   // Main loop
   sip_task_state_t state = 0;
   uint32_t retry = 0;          // Uptime for register retry
   uint32_t backoff = 0;
   while (1)
   {
      sip_state_t status = sip.state;

      uint8_t buf[SIP_MAX];
      int len = 0;
      struct sockaddr_storage addr;
      socklen_t addrlen = 0;

      fd_set r;
      FD_ZERO (&r);
      FD_SET (sock, &r);
      struct timeval t = { 1, 0 };      // Wait 1 second
      if (select (sock + 1, &r, NULL, NULL, &t) > 0)
      {                         // Get packet and process
         addrlen = sizeof (addr);
         len = recvfrom (sock, buf, sizeof (buf) - 1, 0, (struct sockaddr *) &addr, &addrlen);
         if (len > 0)
         {
            ESP_LOGE (TAG, "SIP %d", len);
         }
         continue;
      }

      uint32_t now = uptime ();

      // Do registration logic
      if (sip.regexpiry < now)
         sip.regexpiry = 0;     // Actually expired
      if (sip.regexpiry < now + 60 && sip.ichost && retry < now)
      {
         if (!(addrlen = gethost (sip.ichost, SIP_PORT, &addr)))
            ESP_LOGE (TAG, "Failed to lookup %s", sip.ichost);
         else
         {                      // Send registration
            len = sip_request (buf, &addr, addrlen, "REGISTER", sip.ichost);
            if (!len || sendto (sock, buf, len, 0, (struct sockaddr *) &addr, addrlen) < 0)
               ESP_LOGE (TAG, "Failed to send SIP (%d)", (int) len);
         }
         if (!backoff)
            backoff = 1;
         retry = now + backoff;
         if (backoff < 300)
            backoff *= 2;
      }
      // TODO giveup logic

      // Do periodic
      switch (state)
      {
      case TASK_IDLE:          // Not in a call
         status = SIP_IDLE;
         if (sip.call)
         {                      // Start outgoing call

            sip.call = 0;
         }
         break;
      case TASK_OG_INVITE:     // We are sending INVITEs awaiting any response
         status = SIP_IDLE;
         break;
      case TASK_OG_WAIT:       // We have 1XX and waiting, we will send CANCELs if hangup set
         status = SIP_OG_ALERT;
         break;
      case TASK_OG:            // We are in an outgoing call
         status = SIP_OG;
         break;
      case TASK_OG_BYE:        // We are sending BYEs, awaiting reply
         status = SIP_IDLE;
         break;
      case TASK_IC_ALERT:      // We are sending 180
         status = SIP_IC_ALERT;
         break;
      case TASK_IC_BUSY:       // We are sending 486, waiting ACK
         status = SIP_IDLE;
         break;
      case TASK_IC_OK:         // We are sending 200, waiting ACK
         status = SIP_IC;
         break;
      case TASK_IC:            // We are in an incoming call
         status = SIP_IC;
         break;
      case TASK_IC_BYE:        // We are sendin BYEs, awaiting reply
         status = SIP_IDLE;
         break;
      }

      // Report status change
      if (status == SIP_IDLE && sip.regexpiry)
         status = SIP_REGISTERED;
      if (status == SIP_REGISTERED && !sip.regexpiry)
         status = SIP_IDLE;
      if (status <= SIP_REGISTERED)
         sip.answer = sip.hangup = 0;
      if (sip.state != status && sip.callback)
         sip.callback (sip.state = status, 0, NULL);
   }
}

static void
sip_audio_task (void *arg)
{
   // Set up sockets
   int sock = socket (AF_INET, SOCK_DGRAM, IPPROTO_IP);
   if (sock < 0)
   {
      ESP_LOGE (TAG, "RTP Socket failed");
      vTaskDelete (NULL);
      return;
   }
   struct sockaddr_in dest_addr = {.sin_addr.s_addr = htonl (INADDR_ANY),.sin_family = AF_INET,.sin_port = htons (SIP_RTP)
   };
   if (bind (sock, (struct sockaddr *) &dest_addr, sizeof (dest_addr)))
   {
      ESP_LOGE (TAG, "RTP Bind failed");
      vTaskDelete (NULL);
      return;
   }
   while (1)
   {
      uint8_t buf[SIP_MAX];
      struct sockaddr_storage source_addr;
      socklen_t socklen = sizeof (source_addr);
      int res = recvfrom (sock, buf, sizeof (buf) - 1, 0, (struct sockaddr *) &source_addr, &socklen);
      ESP_LOGE (TAG, "RTP %d", res);
   }
}

// Send audio data for active call
int
sip_audio (uint8_t len, const uint8_t * data)
{
   // TODO
   return 0;
}
