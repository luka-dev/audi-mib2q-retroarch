/* dsi_client_selftest — exercises the SHIPPING module (src/audio/dsi_audio_client.c),
 * not the probes it was distilled from. Same sequence the QSA driver performs:
 * bring up the agent, register the reply service, bind the proxy, ask for the
 * entertainment connection, then a read-only query, and wait for replies. */
#include <stdio.h>
#include <unistd.h>
#include "../src/audio/dsi_audio_client.h"

static void on_reply(int id, int a, int b, void *user)
{
   (void)user;
   printf("   <<< reply id=%d a=%d b=%d\n", id, a, b);
   fflush(stdout);
}

int main(int argc, char **argv)
{
   const char *name = (argc > 1) ? argv[1] : "RetroArch";
   int rc;

   printf("=== dsi_client_selftest (the shipping module) ===\n");
   fflush(stdout);

   rc = dsi_audio_init(name, on_reply, NULL);
   printf("dsi_audio_init -> %d (alive=%d)\n", rc, dsi_audio_is_alive());
   fflush(stdout);
   if (rc != 0) { printf("=== selftest FAILED at init ===\n"); return 1; }

   rc = dsi_audio_get_active_entertainment(0);
   printf("getActiveEntertainmentConnection(id 8) -> %d\n", rc);
   fflush(stdout);

   rc = dsi_audio_request_connection(DSI_VCHANNEL_ENT_INTMEDIA, 0, 0);
   printf("requestConnection(id 12, ENT_INTMEDIA) -> %d\n", rc);
   fflush(stdout);

   sleep(3);
   rc = dsi_audio_release_connection(0, 0);
   printf("releaseConnection(id 11) -> %d\n", rc);
   printf("=== selftest done ===\n");
   fflush(NULL);
   _exit(0);
}
